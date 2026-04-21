// Copyright (c) az_mqtt5_client contributors. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @brief MQTT 5.0 client implementation.
 *
 * Wires together the codec and transport layers. Handles connection,
 * keep-alive, packet framing, and callback dispatch. Zero dynamic allocation.
 */

#include <az_mqtt5/az_mqtt5_client.h>

#include <azure/core/az_platform.h>
#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

static uint16_t _next_packet_id(az_mqtt5_client* client)
{
  client->next_packet_id++;
  if (client->next_packet_id == 0)
  {
    client->next_packet_id = 1;
  }
  return client->next_packet_id;
}

static int64_t _get_clock_ms(void)
{
  int64_t now = 0;
  az_result rc = az_platform_clock_msec(&now);
  (void)rc;
  return now;
}

// Send the contents of the send buffer (from start to the write cursor).
// The encoder writes starting from the beginning and advances `dest`.
// So: bytes_written = original_size - az_span_size(dest_after_encode).
static az_result _send_encoded(az_mqtt5_client* client, az_span original_buf, az_span remaining)
{
  int32_t written = az_span_size(original_buf) - az_span_size(remaining);
  if (written <= 0)
  {
    return AZ_OK;
  }
  az_span to_send = az_span_slice(original_buf, 0, written);
  az_result rc = az_mqtt5_transport_send(client->options.transport, to_send);
  if (az_result_succeeded(rc))
  {
    client->last_send_time_ms = _get_clock_ms();
  }
  return rc;
}

// Ensure we have at least `needed` bytes in the receive buffer.
// Handles partial reads and buffering.
static az_result _ensure_received(az_mqtt5_client* client, int32_t needed, int32_t timeout_ms)
{
  while (client->recv_buf_pos < needed)
  {
    az_span free_space = az_span_slice_to_end(client->options.receive_buffer, client->recv_buf_pos);
    if (az_span_size(free_space) == 0)
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }

    az_span received;
    az_result rc = az_mqtt5_transport_receive(
        client->options.transport, free_space, timeout_ms, &received);
    if (az_result_failed(rc))
    {
      return rc;
    }
    if (az_span_size(received) == 0)
    {
      return AZ_MQTT5_ERROR_TIMEOUT;
    }
    client->recv_buf_pos += az_span_size(received);
  }
  return AZ_OK;
}

// Consume `count` bytes from the front of the receive buffer.
static void _consume_recv(az_mqtt5_client* client, int32_t count)
{
  if (count <= 0)
  {
    return;
  }
  int32_t remaining = client->recv_buf_pos - count;
  if (remaining > 0)
  {
    memmove(
        az_span_ptr(client->options.receive_buffer),
        az_span_ptr(client->options.receive_buffer) + count,
        (size_t)remaining);
  }
  client->recv_buf_pos = remaining;
}

// Read exactly one MQTT packet from the transport. Returns the body span and metadata.
// The caller must call _consume_recv(client, *out_packet_size) AFTER processing the body.
static az_result _read_packet(
    az_mqtt5_client* client,
    int32_t timeout_ms,
    az_mqtt5_packet_type* out_type,
    uint8_t* out_flags,
    az_span* out_body,
    int32_t* out_packet_size)
{
  // We need at least 2 bytes for the fixed header (type + min 1-byte VBI)
  az_result rc = _ensure_received(client, 2, timeout_ms);
  if (az_result_failed(rc))
    return rc;

  // Parse the variable-length remaining length from the buffer.
  // The fixed header is 1 byte + 1-4 bytes VBI.
  int32_t header_size = 1;
  int32_t remaining_length = 0;
  int shift = 0;
  bool vbi_complete = false;

  for (int i = 0; i < 4; i++)
  {
    rc = _ensure_received(client, header_size + 1 + i, timeout_ms);
    if (az_result_failed(rc))
      return rc;

    uint8_t b = az_span_ptr(client->options.receive_buffer)[1 + i];
    remaining_length |= (int32_t)(b & 0x7F) << shift;
    shift += 7;
    if ((b & 0x80) == 0)
    {
      header_size = 2 + i; // 1 (type) + (i+1) VBI bytes
      vbi_complete = true;
      break;
    }
  }

  if (!vbi_complete)
  {
    return AZ_MQTT5_ERROR_MALFORMED_PACKET;
  }

  // Now ensure we have the full packet
  int32_t total_packet_size = header_size + remaining_length;
  rc = _ensure_received(client, total_packet_size, timeout_ms);
  if (az_result_failed(rc))
    return rc;

  // Decode the fixed header using the codec (from a copy of the span)
  az_span packet_span = az_span_slice(client->options.receive_buffer, 0, total_packet_size);
  az_span header_span = packet_span;
  int32_t decoded_remaining;
  rc = az_mqtt5_codec_decode_fixed_header(&header_span, out_type, out_flags, &decoded_remaining);
  if (az_result_failed(rc))
    return rc;

  // header_span now points past the fixed header to the body
  *out_body = az_span_slice(header_span, 0, decoded_remaining);

  // Return packet size so caller can consume AFTER processing the body.
  *out_packet_size = total_packet_size;
  client->last_receive_time_ms = _get_clock_ms();

  return AZ_OK;
}

// ============================================================================
// Packet dispatch
// ============================================================================

static az_result _handle_connack(az_mqtt5_client* client, az_span body)
{
  az_mqtt5_connack_data connack;
  connack.user_properties = client->options.connack_user_properties;
  connack.user_property_count = 0;
  connack.user_property_capacity = client->options.connack_user_property_capacity;

  az_result rc = az_mqtt5_codec_decode_connack(body, &connack);
  if (az_result_failed(rc))
    return rc;

  if (connack.reason_code == AZ_MQTT5_REASON_SUCCESS)
  {
    client->state = AZ_MQTT5_CLIENT_STATE_CONNECTED;
  }

  if (client->options.on_connack != NULL)
  {
    client->options.on_connack(client, &connack);
  }

  return AZ_OK;
}

static az_result _handle_publish(az_mqtt5_client* client, az_span body, uint8_t flags)
{
  az_mqtt5_publish_data publish;
  publish.user_properties = client->options.publish_user_properties;
  publish.user_property_count = 0;
  publish.subscription_identifiers = client->options.publish_subscription_identifiers;
  publish.subscription_identifier_count = 0;

  az_result rc = az_mqtt5_codec_decode_publish(body, flags, &publish);
  if (az_result_failed(rc))
    return rc;

  // Send acknowledgment for QoS > 0
  if (publish.qos == AZ_MQTT5_QOS_AT_LEAST_ONCE)
  {
    az_span send_buf = client->options.send_buffer;
    rc = az_mqtt5_codec_encode_puback(&send_buf, publish.packet_id, AZ_MQTT5_REASON_SUCCESS);
    if (az_result_failed(rc))
      return rc;
    rc = _send_encoded(client, client->options.send_buffer, send_buf);
    if (az_result_failed(rc))
      return rc;
  }
  else if (publish.qos == AZ_MQTT5_QOS_EXACTLY_ONCE)
  {
    az_span send_buf = client->options.send_buffer;
    rc = az_mqtt5_codec_encode_pubrec(&send_buf, publish.packet_id, AZ_MQTT5_REASON_SUCCESS);
    if (az_result_failed(rc))
      return rc;
    rc = _send_encoded(client, client->options.send_buffer, send_buf);
    if (az_result_failed(rc))
      return rc;
  }

  if (client->options.on_publish != NULL)
  {
    client->options.on_publish(client, &publish);
  }

  return AZ_OK;
}

static az_result _handle_puback(az_mqtt5_client* client, az_span body)
{
  az_mqtt5_ack_data ack;
  ack.user_properties = client->options.ack_user_properties;
  ack.user_property_count = 0;
  ack.user_property_capacity = client->options.ack_user_property_capacity;

  az_result rc = az_mqtt5_codec_decode_ack(body, &ack);
  if (az_result_failed(rc))
    return rc;

  if (client->options.on_puback != NULL)
  {
    client->options.on_puback(client, &ack);
  }
  return AZ_OK;
}

static az_result _handle_pubrec(az_mqtt5_client* client, az_span body)
{
  az_mqtt5_ack_data ack;
  ack.user_properties = client->options.ack_user_properties;
  ack.user_property_count = 0;
  ack.user_property_capacity = client->options.ack_user_property_capacity;

  az_result rc = az_mqtt5_codec_decode_ack(body, &ack);
  if (az_result_failed(rc))
    return rc;

  // Send PUBREL
  az_span send_buf = client->options.send_buffer;
  rc = az_mqtt5_codec_encode_pubrel(&send_buf, ack.packet_id, AZ_MQTT5_REASON_SUCCESS);
  if (az_result_failed(rc))
    return rc;
  rc = _send_encoded(client, client->options.send_buffer, send_buf);
  if (az_result_failed(rc))
    return rc;

  return AZ_OK;
}

static az_result _handle_pubrel(az_mqtt5_client* client, az_span body)
{
  az_mqtt5_ack_data ack;
  ack.user_properties = client->options.ack_user_properties;
  ack.user_property_count = 0;
  ack.user_property_capacity = client->options.ack_user_property_capacity;

  az_result rc = az_mqtt5_codec_decode_ack(body, &ack);
  if (az_result_failed(rc))
    return rc;

  // Send PUBCOMP
  az_span send_buf = client->options.send_buffer;
  rc = az_mqtt5_codec_encode_pubcomp(&send_buf, ack.packet_id, AZ_MQTT5_REASON_SUCCESS);
  if (az_result_failed(rc))
    return rc;
  rc = _send_encoded(client, client->options.send_buffer, send_buf);
  if (az_result_failed(rc))
    return rc;

  if (client->options.on_pubcomp != NULL)
  {
    client->options.on_pubcomp(client, &ack);
  }

  return AZ_OK;
}

static az_result _handle_pubcomp(az_mqtt5_client* client, az_span body)
{
  az_mqtt5_ack_data ack;
  ack.user_properties = client->options.ack_user_properties;
  ack.user_property_count = 0;
  ack.user_property_capacity = client->options.ack_user_property_capacity;

  az_result rc = az_mqtt5_codec_decode_ack(body, &ack);
  if (az_result_failed(rc))
    return rc;

  if (client->options.on_pubcomp != NULL)
  {
    client->options.on_pubcomp(client, &ack);
  }
  return AZ_OK;
}

static az_result _handle_suback(az_mqtt5_client* client, az_span body)
{
  az_mqtt5_suback_data suback;
  suback.reason_codes = client->options.suback_reason_codes;
  suback.reason_code_count = 0;
  suback.reason_code_capacity = client->options.suback_reason_code_capacity;
  suback.user_properties = client->options.suback_user_properties;
  suback.user_property_count = 0;
  suback.user_property_capacity = client->options.suback_user_property_capacity;

  az_result rc = az_mqtt5_codec_decode_suback(body, &suback);
  if (az_result_failed(rc))
    return rc;

  if (client->options.on_suback != NULL)
  {
    client->options.on_suback(client, &suback);
  }
  return AZ_OK;
}

static az_result _handle_unsuback(az_mqtt5_client* client, az_span body)
{
  az_mqtt5_suback_data unsuback;
  unsuback.reason_codes = client->options.suback_reason_codes;
  unsuback.reason_code_count = 0;
  unsuback.reason_code_capacity = client->options.suback_reason_code_capacity;
  unsuback.user_properties = client->options.suback_user_properties;
  unsuback.user_property_count = 0;
  unsuback.user_property_capacity = client->options.suback_user_property_capacity;

  az_result rc = az_mqtt5_codec_decode_unsuback(body, &unsuback);
  if (az_result_failed(rc))
    return rc;

  if (client->options.on_unsuback != NULL)
  {
    client->options.on_unsuback(client, &unsuback);
  }
  return AZ_OK;
}

static az_result _handle_disconnect(az_mqtt5_client* client, az_span body)
{
  az_mqtt5_disconnect_data disc;
  disc.user_properties = client->options.disconnect_user_properties;
  disc.user_property_count = 0;
  disc.user_property_capacity = client->options.disconnect_user_property_capacity;

  az_result rc = az_mqtt5_codec_decode_disconnect(body, &disc);
  if (az_result_failed(rc))
    return rc;

  client->state = AZ_MQTT5_CLIENT_STATE_DISCONNECTED;

  if (client->options.on_disconnect != NULL)
  {
    client->options.on_disconnect(client, &disc);
  }
  return AZ_OK;
}

static az_result _dispatch_packet(
    az_mqtt5_client* client,
    az_mqtt5_packet_type type,
    uint8_t flags,
    az_span body)
{
  switch (type)
  {
    case AZ_MQTT5_PACKET_TYPE_CONNACK:
      return _handle_connack(client, body);
    case AZ_MQTT5_PACKET_TYPE_PUBLISH:
      return _handle_publish(client, body, flags);
    case AZ_MQTT5_PACKET_TYPE_PUBACK:
      return _handle_puback(client, body);
    case AZ_MQTT5_PACKET_TYPE_PUBREC:
      return _handle_pubrec(client, body);
    case AZ_MQTT5_PACKET_TYPE_PUBREL:
      return _handle_pubrel(client, body);
    case AZ_MQTT5_PACKET_TYPE_PUBCOMP:
      return _handle_pubcomp(client, body);
    case AZ_MQTT5_PACKET_TYPE_SUBACK:
      return _handle_suback(client, body);
    case AZ_MQTT5_PACKET_TYPE_UNSUBACK:
      return _handle_unsuback(client, body);
    case AZ_MQTT5_PACKET_TYPE_PINGRESP:
      return AZ_OK; // Nothing to do
    case AZ_MQTT5_PACKET_TYPE_DISCONNECT:
      return _handle_disconnect(client, body);
    case AZ_MQTT5_PACKET_TYPE_AUTH:
      // AUTH packets could be handled here for enhanced auth
      return AZ_OK;
    default:
      return AZ_MQTT5_ERROR_PROTOCOL;
  }
}

// ============================================================================
// Public API
// ============================================================================

AZ_NODISCARD az_result az_mqtt5_client_init(az_mqtt5_client* client, az_mqtt5_client_options const* options)
{
  _az_PRECONDITION_NOT_NULL(client);
  _az_PRECONDITION_NOT_NULL(options);
  _az_PRECONDITION_NOT_NULL(options->transport);

  memset(client, 0, sizeof(*client));
  client->options = *options;
  client->state = AZ_MQTT5_CLIENT_STATE_DISCONNECTED;
  client->next_packet_id = 0;
  client->recv_buf_pos = 0;
  client->last_send_time_ms = 0;
  client->last_receive_time_ms = 0;

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_client_connect(az_mqtt5_client* client, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(client);

  if (client->state != AZ_MQTT5_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_MQTT5_ERROR_INVALID_STATE;
  }

  // TCP/TLS connect
  az_result rc = az_mqtt5_transport_connect(
      client->options.transport,
      client->options.hostname,
      client->options.port,
      client->options.tls_options);
  if (az_result_failed(rc))
  {
    return rc;
  }

  // Encode and send CONNECT
  az_span send_buf = client->options.send_buffer;
  rc = az_mqtt5_codec_encode_connect(&send_buf, &client->options.connect_options);
  if (az_result_failed(rc))
  {
    az_mqtt5_transport_close(client->options.transport);
    return rc;
  }

  rc = _send_encoded(client, client->options.send_buffer, send_buf);
  if (az_result_failed(rc))
  {
    az_mqtt5_transport_close(client->options.transport);
    return rc;
  }

  client->state = AZ_MQTT5_CLIENT_STATE_CONNECTING;

  // Wait for CONNACK
  az_mqtt5_packet_type type;
  uint8_t flags;
  az_span body;
  int32_t packet_size;

  rc = _read_packet(client, timeout_ms, &type, &flags, &body, &packet_size);
  if (az_result_failed(rc))
  {
    az_mqtt5_transport_close(client->options.transport);
    client->state = AZ_MQTT5_CLIENT_STATE_DISCONNECTED;
    return rc;
  }

  if (type != AZ_MQTT5_PACKET_TYPE_CONNACK)
  {
    _consume_recv(client, packet_size);
    az_mqtt5_transport_close(client->options.transport);
    client->state = AZ_MQTT5_CLIENT_STATE_DISCONNECTED;
    return AZ_MQTT5_ERROR_PROTOCOL;
  }

  rc = _dispatch_packet(client, type, flags, body);
  _consume_recv(client, packet_size);
  if (az_result_failed(rc))
  {
    az_mqtt5_transport_close(client->options.transport);
    client->state = AZ_MQTT5_CLIENT_STATE_DISCONNECTED;
    return rc;
  }

  // Check if CONNACK was successful
  if (client->state != AZ_MQTT5_CLIENT_STATE_CONNECTED)
  {
    az_mqtt5_transport_close(client->options.transport);
    client->state = AZ_MQTT5_CLIENT_STATE_DISCONNECTED;
    return AZ_MQTT5_ERROR_NOT_CONNECTED;
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_client_process_loop(az_mqtt5_client* client, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(client);

  if (client->state == AZ_MQTT5_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_MQTT5_ERROR_NOT_CONNECTED;
  }

  // Send PINGREQ if needed
  if (client->state == AZ_MQTT5_CLIENT_STATE_CONNECTED
      && client->options.connect_options.keep_alive_seconds > 0)
  {
    int64_t now = _get_clock_ms();
    int64_t keep_alive_ms = (int64_t)client->options.connect_options.keep_alive_seconds * 1000;

    if ((now - client->last_send_time_ms) >= keep_alive_ms)
    {
      az_span send_buf = client->options.send_buffer;
      az_result rc = az_mqtt5_codec_encode_pingreq(&send_buf);
      if (az_result_failed(rc))
        return rc;
      rc = _send_encoded(client, client->options.send_buffer, send_buf);
      if (az_result_failed(rc))
        return rc;
    }
  }

  // Try to read a packet
  az_mqtt5_packet_type type;
  uint8_t flags;
  az_span body;
  int32_t packet_size;

  az_result rc = _read_packet(client, timeout_ms, &type, &flags, &body, &packet_size);
  if (rc == AZ_MQTT5_ERROR_TIMEOUT)
  {
    return AZ_OK; // No data available, that's fine
  }
  if (az_result_failed(rc))
  {
    return rc;
  }

  rc = _dispatch_packet(client, type, flags, body);
  _consume_recv(client, packet_size);
  return rc;
}

AZ_NODISCARD az_result az_mqtt5_client_publish(
    az_mqtt5_client* client,
    az_mqtt5_publish_options const* options,
    uint16_t* out_packet_id)
{
  _az_PRECONDITION_NOT_NULL(client);
  _az_PRECONDITION_NOT_NULL(options);

  if (client->state != AZ_MQTT5_CLIENT_STATE_CONNECTED)
  {
    return AZ_MQTT5_ERROR_NOT_CONNECTED;
  }

  uint16_t packet_id = 0;
  if (options->qos != AZ_MQTT5_QOS_AT_MOST_ONCE)
  {
    packet_id = _next_packet_id(client);
  }

  az_span send_buf = client->options.send_buffer;
  az_result rc = az_mqtt5_codec_encode_publish(&send_buf, options, packet_id);
  if (az_result_failed(rc))
  {
    return rc;
  }

  rc = _send_encoded(client, client->options.send_buffer, send_buf);
  if (az_result_failed(rc))
  {
    return rc;
  }

  if (out_packet_id != NULL)
  {
    *out_packet_id = packet_id;
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_client_subscribe(
    az_mqtt5_client* client,
    az_mqtt5_subscription const* subscriptions,
    int32_t subscription_count,
    uint16_t* out_packet_id)
{
  _az_PRECONDITION_NOT_NULL(client);
  _az_PRECONDITION_NOT_NULL(subscriptions);

  if (client->state != AZ_MQTT5_CLIENT_STATE_CONNECTED)
  {
    return AZ_MQTT5_ERROR_NOT_CONNECTED;
  }

  uint16_t packet_id = _next_packet_id(client);

  az_span send_buf = client->options.send_buffer;
  az_result rc
      = az_mqtt5_codec_encode_subscribe(&send_buf, subscriptions, subscription_count, packet_id);
  if (az_result_failed(rc))
  {
    return rc;
  }

  rc = _send_encoded(client, client->options.send_buffer, send_buf);
  if (az_result_failed(rc))
  {
    return rc;
  }

  if (out_packet_id != NULL)
  {
    *out_packet_id = packet_id;
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_client_unsubscribe(
    az_mqtt5_client* client,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t* out_packet_id)
{
  _az_PRECONDITION_NOT_NULL(client);
  _az_PRECONDITION_NOT_NULL(topic_filters);

  if (client->state != AZ_MQTT5_CLIENT_STATE_CONNECTED)
  {
    return AZ_MQTT5_ERROR_NOT_CONNECTED;
  }

  uint16_t packet_id = _next_packet_id(client);

  az_span send_buf = client->options.send_buffer;
  az_result rc
      = az_mqtt5_codec_encode_unsubscribe(&send_buf, topic_filters, filter_count, packet_id);
  if (az_result_failed(rc))
  {
    return rc;
  }

  rc = _send_encoded(client, client->options.send_buffer, send_buf);
  if (az_result_failed(rc))
  {
    return rc;
  }

  if (out_packet_id != NULL)
  {
    *out_packet_id = packet_id;
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_client_disconnect(
    az_mqtt5_client* client,
    az_mqtt5_reason_code reason_code)
{
  _az_PRECONDITION_NOT_NULL(client);

  if (client->state == AZ_MQTT5_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_OK;
  }

  // Try to send DISCONNECT gracefully
  if (client->state == AZ_MQTT5_CLIENT_STATE_CONNECTED)
  {
    az_span send_buf = client->options.send_buffer;
    az_result rc = az_mqtt5_codec_encode_disconnect(&send_buf, reason_code, 0);
    if (az_result_succeeded(rc))
    {
      _send_encoded(client, client->options.send_buffer, send_buf);
      // Ignore send errors during disconnect
    }
  }

  az_mqtt5_transport_close(client->options.transport);
  client->state = AZ_MQTT5_CLIENT_STATE_DISCONNECTED;
  client->recv_buf_pos = 0;

  return AZ_OK;
}
