// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @brief MQTT client implementation (version-neutral; wire format via az_mqtt_codec).
 *
 * Wires together the codec and transport layers. Handles connection,
 * keep-alive, packet framing, and callback dispatch. Zero dynamic allocation.
 */

#include <az_mqtt/az_mqtt_client.h>

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

static uint16_t _next_packet_id(az_mqtt_client* client)
{
  client->next_packet_id++;
  if (client->next_packet_id == 0)
  {
    client->next_packet_id = 1;
  }
  return client->next_packet_id;
}

/** @brief Most packets handled by one az_mqtt_client_process_loop() call. */
#define _AZ_MQTT_MAX_PACKETS_PER_LOOP 32

static int64_t _get_clock_ms(void) { return az_mqtt_transport_clock_ms(); }

/** @brief Absolute deadline @p timeout_ms from now; -1 (no limit) for a negative timeout. */
static int64_t _deadline(int32_t timeout_ms)
{
  return timeout_ms < 0 ? -1 : _get_clock_ms() + timeout_ms;
}

/** @brief Milliseconds left until @p deadline_ms, 0 if passed, -1 if unlimited. */
static int32_t _remaining(int64_t deadline_ms)
{
  if (deadline_ms < 0)
  {
    return -1;
  }
  int64_t left = deadline_ms - _get_clock_ms();
  return left <= 0 ? 0 : (left > INT32_MAX ? INT32_MAX : (int32_t)left);
}

/**
 * @brief End the session: close the transport, reset state, report why.
 *
 * on_connection_closed runs only if the client was CONNECTING or CONNECTED.
 */
static void _close(az_mqtt_client* client, az_result reason)
{
  bool const was_open = client->state != AZ_MQTT_CLIENT_STATE_DISCONNECTED;
  az_mqtt_transport_close(client->options.transport);
  client->state = AZ_MQTT_CLIENT_STATE_DISCONNECTED;
  client->recv_buf_pos = 0;
  client->ping_outstanding = false;
  // The callback may reconnect: callers compare generations before touching
  // anything that belonged to the old session.
  client->session_generation++;
  if (was_open && client->options.on_connection_closed != NULL)
  {
    client->options.on_connection_closed(client, reason);
  }
}

static int32_t _span_count(az_span span, int32_t element_size)
{
  return (element_size > 0) ? (az_span_size(span) / element_size) : 0;
}

static az_mqtt_user_property* _span_user_properties(az_span span)
{
  return (az_mqtt_user_property*)az_span_ptr(span);
}

static az_mqtt_reason_code* _span_reason_codes(az_span span)
{
  return (az_mqtt_reason_code*)az_span_ptr(span);
}

static int32_t* _span_i32(az_span span)
{
  return (int32_t*)az_span_ptr(span);
}

// Send the contents of the send buffer (from start to the write cursor).
// The encoder writes starting from the beginning and advances `dest`.
// So: bytes_written = original_size - az_span_size(dest_after_encode).
static az_result _send_encoded(az_mqtt_client* client, az_span original_buf, az_span remaining)
{
  int32_t written = az_span_size(original_buf) - az_span_size(remaining);
  if (written <= 0)
  {
    return AZ_OK;
  }
  az_span to_send = az_span_slice(original_buf, 0, written);
  az_result rc = az_mqtt_transport_send(client->options.transport, to_send);
  if (az_result_succeeded(rc))
  {
    client->last_send_time_ms = _get_clock_ms();
  }
  return rc;
}

// Ensure we have at least `needed` bytes in the receive buffer, waiting no
// later than `deadline_ms`. Handles partial reads and buffering.
static az_result _ensure_received(az_mqtt_client* client, int32_t needed, int64_t deadline_ms)
{
  while (client->recv_buf_pos < needed)
  {
    az_span free_space = az_span_slice_to_end(client->options.receive_buffer, client->recv_buf_pos);
    if (az_span_size(free_space) == 0)
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }

    int32_t const wait_ms = _remaining(deadline_ms);
    az_span received;
    az_result rc = az_mqtt_transport_receive(
        client->options.transport, free_space, wait_ms, &received);
    if (az_result_failed(rc))
    {
      return rc;
    }
    if (az_span_size(received) == 0)
    {
      if (wait_ms == 0)
      {
        return AZ_MQTT_ERROR_TIMEOUT;
      }
      continue; // Woke early; wait out the rest of the deadline.
    }
    client->recv_buf_pos += az_span_size(received);
  }
  return AZ_OK;
}

// Consume `count` bytes from the front of the receive buffer.
static void _consume_recv(az_mqtt_client* client, int32_t count)
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
    az_mqtt_client* client,
    int64_t deadline_ms,
    az_mqtt_packet_type* out_type,
    uint8_t* out_flags,
    az_span* out_body,
    int32_t* out_packet_size)
{
  // We need at least 2 bytes for the fixed header (type + min 1-byte VBI)
  az_result rc = _ensure_received(client, 2, deadline_ms);
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
    rc = _ensure_received(client, header_size + 1 + i, deadline_ms);
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
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }

  // Now ensure we have the full packet
  int32_t total_packet_size = header_size + remaining_length;
  rc = _ensure_received(client, total_packet_size, deadline_ms);
  if (az_result_failed(rc))
    return rc;

  // Decode the fixed header using the codec (from a copy of the span)
  az_span packet_span = az_span_slice(client->options.receive_buffer, 0, total_packet_size);
  az_span header_span = packet_span;
  int32_t decoded_remaining;
  rc = az_mqtt_codec_decode_fixed_header(&header_span, out_type, out_flags, &decoded_remaining);
  if (az_result_failed(rc))
    return rc;

  // header_span now points past the fixed header to the body
  *out_body = az_span_slice(header_span, 0, decoded_remaining);

  // Return packet size so caller can consume AFTER processing the body.
  *out_packet_size = total_packet_size;
  client->last_receive_time_ms = _get_clock_ms();
  // Any packet proves the link is alive, not only a PINGRESP.
  client->ping_outstanding = false;

  return AZ_OK;
}

// ============================================================================
// Packet dispatch
// ============================================================================

static az_result _handle_connack(az_mqtt_client* client, az_span body)
{
  az_mqtt_connack_data connack;
  connack.user_properties = _span_user_properties(client->options.buffers.connack_user_properties);
  connack.user_property_count = 0;
  connack.user_property_capacity =
      _span_count(client->options.buffers.connack_user_properties, (int32_t)sizeof(az_mqtt_user_property));

  az_result rc = client->options.codec->decode_connack(body, &connack);
  if (az_result_failed(rc))
    return rc;

  if (connack.reason_code == AZ_MQTT_REASON_SUCCESS)
  {
    client->state = AZ_MQTT_CLIENT_STATE_CONNECTED;
    client->keep_alive_seconds = connack.server_keep_alive_present
        ? connack.server_keep_alive
        : client->options.connect_options.keep_alive_seconds;
  }

  if (client->options.on_connack != NULL)
  {
    client->options.on_connack(client, &connack);
  }

  return AZ_OK;
}

static az_result _handle_publish(az_mqtt_client* client, az_span body, uint8_t flags)
{
  az_mqtt_publish_data publish;
  publish.user_properties = _span_user_properties(client->options.buffers.publish_user_properties);
  publish.user_property_count = 0;
  publish.user_property_capacity
      = _span_count(client->options.buffers.publish_user_properties, (int32_t)sizeof(az_mqtt_user_property));
  publish.subscription_identifiers = _span_i32(client->options.buffers.publish_subscription_identifiers);
  publish.subscription_identifier_count = 0;
  publish.subscription_identifier_capacity
      = _span_count(client->options.buffers.publish_subscription_identifiers, (int32_t)sizeof(int32_t));

  az_result rc = client->options.codec->decode_publish(body, flags, &publish);
  if (az_result_failed(rc))
    return rc;

  // Send acknowledgment for QoS > 0
  if (publish.qos == AZ_MQTT_QOS_AT_LEAST_ONCE)
  {
    az_span send_buf = client->options.send_buffer;
    rc = client->options.codec->encode_puback(&send_buf, publish.packet_id, AZ_MQTT_REASON_SUCCESS);
    if (az_result_failed(rc))
      return rc;
    rc = _send_encoded(client, client->options.send_buffer, send_buf);
    if (az_result_failed(rc))
      return rc;
  }
  else if (publish.qos == AZ_MQTT_QOS_EXACTLY_ONCE)
  {
    az_span send_buf = client->options.send_buffer;
    rc = client->options.codec->encode_pubrec(&send_buf, publish.packet_id, AZ_MQTT_REASON_SUCCESS);
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

static az_result _handle_puback(az_mqtt_client* client, az_span body)
{
  az_mqtt_ack_data ack;
  ack.user_properties = _span_user_properties(client->options.buffers.ack_user_properties);
  ack.user_property_count = 0;
  ack.user_property_capacity =
      _span_count(client->options.buffers.ack_user_properties, (int32_t)sizeof(az_mqtt_user_property));

  az_result rc = client->options.codec->decode_ack(body, &ack);
  if (az_result_failed(rc))
    return rc;

  if (client->options.on_puback != NULL)
  {
    client->options.on_puback(client, &ack);
  }
  return AZ_OK;
}

static az_result _handle_pubrec(az_mqtt_client* client, az_span body)
{
  az_mqtt_ack_data ack;
  ack.user_properties = _span_user_properties(client->options.buffers.ack_user_properties);
  ack.user_property_count = 0;
  ack.user_property_capacity =
      _span_count(client->options.buffers.ack_user_properties, (int32_t)sizeof(az_mqtt_user_property));

  az_result rc = client->options.codec->decode_ack(body, &ack);
  if (az_result_failed(rc))
    return rc;

  // Send PUBREL
  az_span send_buf = client->options.send_buffer;
  rc = client->options.codec->encode_pubrel(&send_buf, ack.packet_id, AZ_MQTT_REASON_SUCCESS);
  if (az_result_failed(rc))
    return rc;
  rc = _send_encoded(client, client->options.send_buffer, send_buf);
  if (az_result_failed(rc))
    return rc;

  return AZ_OK;
}

static az_result _handle_pubrel(az_mqtt_client* client, az_span body)
{
  az_mqtt_ack_data ack;
  ack.user_properties = _span_user_properties(client->options.buffers.ack_user_properties);
  ack.user_property_count = 0;
  ack.user_property_capacity =
      _span_count(client->options.buffers.ack_user_properties, (int32_t)sizeof(az_mqtt_user_property));

  az_result rc = client->options.codec->decode_ack(body, &ack);
  if (az_result_failed(rc))
    return rc;

  // Send PUBCOMP
  az_span send_buf = client->options.send_buffer;
  rc = client->options.codec->encode_pubcomp(&send_buf, ack.packet_id, AZ_MQTT_REASON_SUCCESS);
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

static az_result _handle_pubcomp(az_mqtt_client* client, az_span body)
{
  az_mqtt_ack_data ack;
  ack.user_properties = _span_user_properties(client->options.buffers.ack_user_properties);
  ack.user_property_count = 0;
  ack.user_property_capacity =
      _span_count(client->options.buffers.ack_user_properties, (int32_t)sizeof(az_mqtt_user_property));

  az_result rc = client->options.codec->decode_ack(body, &ack);
  if (az_result_failed(rc))
    return rc;

  if (client->options.on_pubcomp != NULL)
  {
    client->options.on_pubcomp(client, &ack);
  }
  return AZ_OK;
}

static az_result _handle_suback(az_mqtt_client* client, az_span body)
{
  az_mqtt_suback_data suback;
  suback.reason_codes = _span_reason_codes(client->options.buffers.suback_reason_codes);
  suback.reason_code_count = 0;
  suback.reason_code_capacity =
      _span_count(client->options.buffers.suback_reason_codes, (int32_t)sizeof(az_mqtt_reason_code));
  suback.user_properties = _span_user_properties(client->options.buffers.suback_user_properties);
  suback.user_property_count = 0;
  suback.user_property_capacity =
      _span_count(client->options.buffers.suback_user_properties, (int32_t)sizeof(az_mqtt_user_property));

  az_result rc = client->options.codec->decode_suback(body, &suback);
  if (az_result_failed(rc))
    return rc;

  if (client->options.on_suback != NULL)
  {
    client->options.on_suback(client, &suback);
  }
  return AZ_OK;
}

static az_result _handle_unsuback(az_mqtt_client* client, az_span body)
{
  az_mqtt_suback_data unsuback;
  unsuback.reason_codes = _span_reason_codes(client->options.buffers.suback_reason_codes);
  unsuback.reason_code_count = 0;
  unsuback.reason_code_capacity =
      _span_count(client->options.buffers.suback_reason_codes, (int32_t)sizeof(az_mqtt_reason_code));
  unsuback.user_properties = _span_user_properties(client->options.buffers.suback_user_properties);
  unsuback.user_property_count = 0;
  unsuback.user_property_capacity =
      _span_count(client->options.buffers.suback_user_properties, (int32_t)sizeof(az_mqtt_user_property));

  az_result rc = client->options.codec->decode_unsuback(body, &unsuback);
  if (az_result_failed(rc))
    return rc;

  if (client->options.on_unsuback != NULL)
  {
    client->options.on_unsuback(client, &unsuback);
  }
  return AZ_OK;
}

static az_result _handle_disconnect(az_mqtt_client* client, az_span body)
{
  az_mqtt_disconnect_data disc;
  disc.user_properties = _span_user_properties(client->options.buffers.disconnect_user_properties);
  disc.user_property_count = 0;
  disc.user_property_capacity = _span_count(
      client->options.buffers.disconnect_user_properties, (int32_t)sizeof(az_mqtt_user_property));

  az_result rc = client->options.codec->decode_disconnect(body, &disc);
  if (az_result_failed(rc))
    return rc;

  if (client->options.on_disconnect != NULL)
  {
    client->options.on_disconnect(client, &disc);
  }
  _close(client, AZ_MQTT_ERROR_SERVER_DISCONNECTED);
  return AZ_OK;
}

static az_result _dispatch_packet(
    az_mqtt_client* client,
    az_mqtt_packet_type type,
    uint8_t flags,
    az_span body)
{
  switch (type)
  {
    case AZ_MQTT_PACKET_TYPE_CONNACK:
      return _handle_connack(client, body);
    case AZ_MQTT_PACKET_TYPE_PUBLISH:
      return _handle_publish(client, body, flags);
    case AZ_MQTT_PACKET_TYPE_PUBACK:
      return _handle_puback(client, body);
    case AZ_MQTT_PACKET_TYPE_PUBREC:
      return _handle_pubrec(client, body);
    case AZ_MQTT_PACKET_TYPE_PUBREL:
      return _handle_pubrel(client, body);
    case AZ_MQTT_PACKET_TYPE_PUBCOMP:
      return _handle_pubcomp(client, body);
    case AZ_MQTT_PACKET_TYPE_SUBACK:
      return _handle_suback(client, body);
    case AZ_MQTT_PACKET_TYPE_UNSUBACK:
      return _handle_unsuback(client, body);
    case AZ_MQTT_PACKET_TYPE_PINGRESP:
      return AZ_OK; // Nothing to do
    case AZ_MQTT_PACKET_TYPE_DISCONNECT:
      return _handle_disconnect(client, body);
    case AZ_MQTT_PACKET_TYPE_AUTH:
      // MQTT 5.0 only; packet type 15 is reserved in 3.1.1. Enhanced auth is not implemented.
      return client->options.codec->protocol_version >= 5 ? AZ_OK : AZ_MQTT_ERROR_PROTOCOL;
    default:
      return AZ_MQTT_ERROR_PROTOCOL;
  }
}

// ============================================================================
// Public API
// ============================================================================

AZ_NODISCARD az_result az_mqtt_client_init(az_mqtt_client* client, az_mqtt_client_options const* options)
{
  _az_PRECONDITION_NOT_NULL(client);
  _az_PRECONDITION_NOT_NULL(options);
  _az_PRECONDITION_NOT_NULL(options->transport);
  _az_PRECONDITION_NOT_NULL(options->codec);

  memset(client, 0, sizeof(*client));
  client->options = *options;
  client->state = AZ_MQTT_CLIENT_STATE_DISCONNECTED;
  client->next_packet_id = 0;
  client->recv_buf_pos = 0;
  client->last_send_time_ms = 0;
  client->last_receive_time_ms = 0;

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt_client_connect(az_mqtt_client* client, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(client);

  if (client->state != AZ_MQTT_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_MQTT_ERROR_INVALID_STATE;
  }

  int64_t const deadline = _deadline(timeout_ms);
  client->recv_buf_pos = 0;
  client->ping_outstanding = false;
  client->state = AZ_MQTT_CLIENT_STATE_CONNECTING;

  // TCP/TLS connect, bounded by the same deadline as the CONNACK.
  az_result rc = az_mqtt_transport_connect_start(
      client->options.transport,
      client->options.hostname,
      client->options.port,
      client->options.tls_options);
  while (rc == AZ_OK)
  {
    rc = az_mqtt_transport_connect_poll(client->options.transport, _remaining(deadline));
    if (rc == AZ_OK)
    {
      break;
    }
    if (rc == AZ_MQTT_ERROR_TIMEOUT && _remaining(deadline) != 0)
    {
      rc = AZ_OK; // Woke early; keep polling until the deadline.
    }
  }
  if (az_result_failed(rc))
  {
    _close(client, rc);
    return rc;
  }

  // Encode and send CONNECT
  az_span send_buf = client->options.send_buffer;
  rc = client->options.codec->encode_connect(&send_buf, &client->options.connect_options);
  if (az_result_succeeded(rc))
  {
    rc = _send_encoded(client, client->options.send_buffer, send_buf);
  }
  if (az_result_failed(rc))
  {
    _close(client, rc);
    return rc;
  }

  // Wait for CONNACK
  az_mqtt_packet_type type;
  uint8_t flags;
  az_span body;
  int32_t packet_size;

  uint32_t const generation = client->session_generation;
  rc = _read_packet(client, deadline, &type, &flags, &body, &packet_size);
  if (az_result_succeeded(rc))
  {
    if (type != AZ_MQTT_PACKET_TYPE_CONNACK)
    {
      rc = AZ_MQTT_ERROR_PROTOCOL;
    }
    else
    {
      rc = _dispatch_packet(client, type, flags, body);
    }
    if (client->session_generation != generation)
    {
      // on_connack ended the session (and may have started another): leave it be.
      return client->state == AZ_MQTT_CLIENT_STATE_CONNECTED ? AZ_OK
                                                                : AZ_MQTT_ERROR_NOT_CONNECTED;
    }
    _consume_recv(client, packet_size);
  }
  if (az_result_succeeded(rc) && client->state != AZ_MQTT_CLIENT_STATE_CONNECTED)
  {
    rc = AZ_MQTT_ERROR_NOT_CONNECTED; // CONNACK refused; the reason went to on_connack.
  }
  if (az_result_failed(rc))
  {
    _close(client, rc);
    return rc;
  }

  client->last_receive_time_ms = _get_clock_ms();
  return AZ_OK;
}

/**
 * @brief Send PINGREQ when due and detect a missing response.
 *
 * @param[out] out_next_ms Milliseconds until keep-alive next needs attention; -1 if never.
 */
static az_result _service_keep_alive(az_mqtt_client* client, int32_t* out_next_ms)
{
  *out_next_ms = -1;
  if (client->state != AZ_MQTT_CLIENT_STATE_CONNECTED || client->keep_alive_seconds == 0)
  {
    return AZ_OK;
  }

  int64_t const now = _get_clock_ms();
  int64_t const keep_alive_ms = (int64_t)client->keep_alive_seconds * 1000;

  if (client->ping_outstanding)
  {
    int64_t const waited = now - client->ping_sent_time_ms;
    if (waited >= keep_alive_ms)
    {
      return AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT;
    }
    *out_next_ms = (int32_t)(keep_alive_ms - waited);
    return AZ_OK;
  }

  int64_t const idle = now - client->last_send_time_ms;
  if (idle < keep_alive_ms)
  {
    *out_next_ms = (int32_t)(keep_alive_ms - idle);
    return AZ_OK;
  }

  az_span send_buf = client->options.send_buffer;
  az_result rc = az_mqtt_codec_encode_pingreq(&send_buf);
  if (az_result_succeeded(rc))
  {
    rc = _send_encoded(client, client->options.send_buffer, send_buf);
  }
  if (az_result_succeeded(rc))
  {
    // The response window starts once the PINGREQ is out, not before a slow send.
    client->ping_outstanding = true;
    client->ping_sent_time_ms = _get_clock_ms();
    *out_next_ms = (int32_t)keep_alive_ms;
  }
  return rc;
}

AZ_NODISCARD az_result az_mqtt_client_process_loop(az_mqtt_client* client, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(client);

  if (client->state == AZ_MQTT_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_MQTT_ERROR_NOT_CONNECTED;
  }

  int32_t next_keep_alive_ms;
  az_result rc = _service_keep_alive(client, &next_keep_alive_ms);
  if (az_result_failed(rc))
  {
    _close(client, rc);
    return rc;
  }

  // Never sleep past the next keep-alive action.
  int32_t wait_ms = timeout_ms;
  if (next_keep_alive_ms >= 0 && (wait_ms < 0 || next_keep_alive_ms < wait_ms))
  {
    wait_ms = next_keep_alive_ms;
  }
  int64_t deadline = _deadline(wait_ms);

  // Handle every complete packet already available, up to a bound.
  for (int i = 0; i < _AZ_MQTT_MAX_PACKETS_PER_LOOP
       && client->state == AZ_MQTT_CLIENT_STATE_CONNECTED;
       i++)
  {
    az_mqtt_packet_type type;
    uint8_t flags;
    az_span body;
    int32_t packet_size;

    uint32_t const generation = client->session_generation;
    rc = _read_packet(client, deadline, &type, &flags, &body, &packet_size);
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      rc = AZ_OK;
      break;
    }
    if (az_result_succeeded(rc))
    {
      rc = _dispatch_packet(client, type, flags, body);
      if (client->session_generation != generation)
      {
        // A callback ended this session, and may have connected a new one whose
        // receive buffer must not be touched: stop here.
        return az_result_failed(rc) ? rc : AZ_OK;
      }
      _consume_recv(client, packet_size);
    }
    if (az_result_failed(rc))
    {
      _close(client, rc);
      return rc;
    }
    deadline = _get_clock_ms(); // Only what is already there from now on.
  }

  // The wait may have been cut to the keep-alive time: act on it now.
  rc = _service_keep_alive(client, &next_keep_alive_ms);
  if (az_result_failed(rc))
  {
    _close(client, rc);
    return rc;
  }
  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt_client_publish(
    az_mqtt_client* client,
    az_mqtt_publish_options const* options,
    uint16_t* out_packet_id)
{
  _az_PRECONDITION_NOT_NULL(client);
  _az_PRECONDITION_NOT_NULL(options);

  if (client->state != AZ_MQTT_CLIENT_STATE_CONNECTED)
  {
    return AZ_MQTT_ERROR_NOT_CONNECTED;
  }

  uint16_t packet_id = 0;
  if (options->qos != AZ_MQTT_QOS_AT_MOST_ONCE)
  {
    packet_id = _next_packet_id(client);
  }

  az_span send_buf = client->options.send_buffer;
  az_result rc = client->options.codec->encode_publish(&send_buf, options, packet_id);
  if (az_result_failed(rc))
  {
    return rc;
  }

  rc = _send_encoded(client, client->options.send_buffer, send_buf);
  if (az_result_failed(rc))
  {
    _close(client, rc); // A partial packet may be on the wire.
    return rc;
  }

  if (out_packet_id != NULL)
  {
    *out_packet_id = packet_id;
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt_client_subscribe(
    az_mqtt_client* client,
    az_mqtt_subscription const* subscriptions,
    int32_t subscription_count,
    uint16_t* out_packet_id)
{
  _az_PRECONDITION_NOT_NULL(client);
  _az_PRECONDITION_NOT_NULL(subscriptions);

  if (client->state != AZ_MQTT_CLIENT_STATE_CONNECTED)
  {
    return AZ_MQTT_ERROR_NOT_CONNECTED;
  }

  uint16_t packet_id = _next_packet_id(client);

  az_span send_buf = client->options.send_buffer;
  az_result rc
      = client->options.codec->encode_subscribe(&send_buf, subscriptions, subscription_count, packet_id);
  if (az_result_failed(rc))
  {
    return rc;
  }

  rc = _send_encoded(client, client->options.send_buffer, send_buf);
  if (az_result_failed(rc))
  {
    _close(client, rc); // A partial packet may be on the wire.
    return rc;
  }

  if (out_packet_id != NULL)
  {
    *out_packet_id = packet_id;
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt_client_unsubscribe(
    az_mqtt_client* client,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t* out_packet_id)
{
  _az_PRECONDITION_NOT_NULL(client);
  _az_PRECONDITION_NOT_NULL(topic_filters);

  if (client->state != AZ_MQTT_CLIENT_STATE_CONNECTED)
  {
    return AZ_MQTT_ERROR_NOT_CONNECTED;
  }

  uint16_t packet_id = _next_packet_id(client);

  az_span send_buf = client->options.send_buffer;
  az_result rc
      = client->options.codec->encode_unsubscribe(&send_buf, topic_filters, filter_count, packet_id);
  if (az_result_failed(rc))
  {
    return rc;
  }

  rc = _send_encoded(client, client->options.send_buffer, send_buf);
  if (az_result_failed(rc))
  {
    _close(client, rc); // A partial packet may be on the wire.
    return rc;
  }

  if (out_packet_id != NULL)
  {
    *out_packet_id = packet_id;
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt_client_disconnect(
    az_mqtt_client* client,
    az_mqtt_reason_code reason_code)
{
  _az_PRECONDITION_NOT_NULL(client);

  if (client->state == AZ_MQTT_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_OK;
  }

  // Try to send DISCONNECT gracefully
  if (client->state == AZ_MQTT_CLIENT_STATE_CONNECTED)
  {
    az_span send_buf = client->options.send_buffer;
    az_result rc = client->options.codec->encode_disconnect(&send_buf, reason_code, 0);
    if (az_result_succeeded(rc))
    {
      // Best effort: the session ends either way.
      (void)_send_encoded(client, client->options.send_buffer, send_buf);
    }
  }

  _close(client, AZ_OK);
  return AZ_OK;
}
