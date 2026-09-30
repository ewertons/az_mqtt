// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @brief MQTT 3.1.1 packet encoding and decoding.
 *
 * All operations use az_span for buffer management. No dynamic allocation.
 * The encoder writes into a caller-provided buffer and advances the span pointer.
 * The decoder reads from a span and produces typed structures.
 */

#include <az_mqtt3/az_mqtt3_codec.h>

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <string.h>

// ============================================================================
// Internal helpers
// ============================================================================

// Write a single byte and advance dest.
static az_result _write_byte(az_span* dest, uint8_t b)
{
  if (az_span_size(*dest) < 1)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  az_span_ptr(*dest)[0] = b;
  *dest = az_span_slice_to_end(*dest, 1);
  return AZ_OK;
}

// Write a 16-bit big-endian value.
static az_result _write_uint16(az_span* dest, uint16_t val)
{
  if (az_span_size(*dest) < 2)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  az_span_ptr(*dest)[0] = (uint8_t)(val >> 8);
  az_span_ptr(*dest)[1] = (uint8_t)(val & 0xFF);
  *dest = az_span_slice_to_end(*dest, 2);
  return AZ_OK;
}

// Write a UTF-8 string (2-byte length prefix + data).
static az_result _write_utf8_string(az_span* dest, az_span str)
{
  int32_t len = az_span_size(str);
  if (len > 65535)
  {
    return AZ_ERROR_ARG;
  }

  az_result rc = _write_uint16(dest, (uint16_t)len);
  if (az_result_failed(rc))
  {
    return rc;
  }

  if (len > 0)
  {
    if (az_span_size(*dest) < len)
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }
    memcpy(az_span_ptr(*dest), az_span_ptr(str), (size_t)len);
    *dest = az_span_slice_to_end(*dest, len);
  }
  return AZ_OK;
}

// Write binary data (2-byte length prefix + data). Same wire format as UTF-8 string.
static az_result _write_binary_data(az_span* dest, az_span data)
{
  return _write_utf8_string(dest, data);
}

// Write a Variable Byte Integer (MQTT VBI encoding, 1-4 bytes).
static az_result _write_vbi(az_span* dest, int32_t value)
{
  if (value < 0 || value > 268435455) // max VBI
  {
    return AZ_ERROR_ARG;
  }
  do
  {
    uint8_t encoded_byte = (uint8_t)(value & 0x7F);
    value >>= 7;
    if (value > 0)
    {
      encoded_byte |= 0x80;
    }
    az_result rc = _write_byte(dest, encoded_byte);
    if (az_result_failed(rc))
    {
      return rc;
    }
  } while (value > 0);
  return AZ_OK;
}

// Calculate the number of bytes needed to encode a VBI.
static int32_t _vbi_size(int32_t value)
{
  if (value <= 127)
    return 1;
  if (value <= 16383)
    return 2;
  if (value <= 2097151)
    return 3;
  return 4;
}

// Read a single byte and advance src.
static az_result _read_byte(az_span* src, uint8_t* out)
{
  if (az_span_size(*src) < 1)
  {
    return AZ_ERROR_UNEXPECTED_END;
  }
  *out = az_span_ptr(*src)[0];
  *src = az_span_slice_to_end(*src, 1);
  return AZ_OK;
}

// Read a 16-bit big-endian value.
static az_result _read_uint16(az_span* src, uint16_t* out)
{
  if (az_span_size(*src) < 2)
  {
    return AZ_ERROR_UNEXPECTED_END;
  }
  *out = (uint16_t)((uint16_t)az_span_ptr(*src)[0] << 8 | (uint16_t)az_span_ptr(*src)[1]);
  *src = az_span_slice_to_end(*src, 2);
  return AZ_OK;
}

// Read a Variable Byte Integer.
static az_result _read_vbi(az_span* src, int32_t* out)
{
  int32_t result = 0;
  int shift = 0;
  for (int i = 0; i < 4; i++)
  {
    uint8_t b;
    az_result rc = _read_byte(src, &b);
    if (az_result_failed(rc))
    {
      return rc;
    }
    result |= (int32_t)(b & 0x7F) << shift;
    if ((b & 0x80) == 0)
    {
      *out = result;
      return AZ_OK;
    }
    shift += 7;
  }
  return AZ_MQTT3_ERROR_MALFORMED_PACKET;
}

// Read a UTF-8 string (returns az_span pointing into the source buffer).
static az_result _read_utf8_string(az_span* src, az_span* out)
{
  uint16_t len;
  az_result rc = _read_uint16(src, &len);
  if (az_result_failed(rc))
  {
    return rc;
  }
  if (az_span_size(*src) < (int32_t)len)
  {
    return AZ_ERROR_UNEXPECTED_END;
  }
  *out = az_span_slice(*src, 0, (int32_t)len);
  *src = az_span_slice_to_end(*src, (int32_t)len);
  return AZ_OK;
}

// Read binary data.
static az_result _read_binary_data(az_span* src, az_span* out)
{
  return _read_utf8_string(src, out);
}

// ============================================================================
// Property encoding helpers
// ============================================================================

// ============================================================================
// CONNECT encoding
// ============================================================================

// Calculate the variable header + payload length of a CONNECT packet.
static int32_t _connect_remaining_length(az_mqtt3_connect_options const* opts)
{
  // Protocol Name (2+4) + Protocol Level (1) + Connect Flags (1) + Keep Alive (2)
  int32_t len = 10;

  // Payload: Client ID
  len += 2 + az_span_size(opts->client_id);

  // Will
  if (opts->will != NULL)
  {
    len += 2 + az_span_size(opts->will->topic); // Will Topic
    len += 2 + az_span_size(opts->will->payload); // Will Payload
  }

  // Username
  if (az_span_size(opts->username) > 0)
  {
    len += 2 + az_span_size(opts->username);
  }

  // Password
  if (az_span_size(opts->password) > 0)
  {
    len += 2 + az_span_size(opts->password);
  }

  return len;
}

AZ_NODISCARD az_result
az_mqtt3_codec_encode_connect(az_span* dest, az_mqtt3_connect_options const* opts)
{
  _az_PRECONDITION_NOT_NULL(dest);
  _az_PRECONDITION_NOT_NULL(opts);

  int32_t remaining = _connect_remaining_length(opts);

  // Fixed header
  az_result rc = _write_byte(dest, (uint8_t)(AZ_MQTT3_PACKET_TYPE_CONNECT << 4));
  if (az_result_failed(rc))
    return rc;
  rc = _write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;

  // Protocol Name "MQTT"
  rc = _write_utf8_string(dest, AZ_SPAN_FROM_STR("MQTT"));
  if (az_result_failed(rc))
    return rc;

  // Protocol Version (4 = MQTT 3.1.1)
  rc = _write_byte(dest, AZ_MQTT3_PROTOCOL_VERSION);
  if (az_result_failed(rc))
    return rc;

  // Connect Flags
  uint8_t flags = 0;
  if (opts->clean_start)
  {
    flags |= 0x02;
  }
  if (opts->will != NULL)
  {
    flags |= 0x04;
    flags |= (uint8_t)((uint8_t)opts->will->qos << 3);
    if (opts->will->retain)
    {
      flags |= 0x20;
    }
  }
  if (az_span_size(opts->password) > 0)
  {
    flags |= 0x40;
  }
  if (az_span_size(opts->username) > 0)
  {
    flags |= 0x80;
  }
  rc = _write_byte(dest, flags);
  if (az_result_failed(rc))
    return rc;

  // Keep Alive
  rc = _write_uint16(dest, opts->keep_alive_seconds);
  if (az_result_failed(rc))
    return rc;

  // Payload: Client ID
  rc = _write_utf8_string(dest, opts->client_id);
  if (az_result_failed(rc))
    return rc;

  // Will
  if (opts->will != NULL)
  {
    rc = _write_utf8_string(dest, opts->will->topic);
    if (az_result_failed(rc))
      return rc;
    rc = _write_binary_data(dest, opts->will->payload);
    if (az_result_failed(rc))
      return rc;
  }

  // Username
  if (az_span_size(opts->username) > 0)
  {
    rc = _write_utf8_string(dest, opts->username);
    if (az_result_failed(rc))
      return rc;
  }

  // Password
  if (az_span_size(opts->password) > 0)
  {
    rc = _write_binary_data(dest, opts->password);
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// PUBLISH encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_encode_publish(
    az_span* dest,
    az_mqtt3_publish_options const* opts,
    uint16_t packet_id)
{
  _az_PRECONDITION_NOT_NULL(dest);
  _az_PRECONDITION_NOT_NULL(opts);

  // Calculate remaining length
  int32_t remaining = 2 + az_span_size(opts->topic); // Topic Name
  if (opts->qos != AZ_MQTT3_QOS_AT_MOST_ONCE)
  {
    remaining += 2; // Packet Identifier
  }
  remaining += az_span_size(opts->payload); // Payload (no length prefix)

  // Fixed header
  uint8_t first_byte = (uint8_t)(AZ_MQTT3_PACKET_TYPE_PUBLISH << 4);
  first_byte |= (uint8_t)((uint8_t)opts->qos << 1);
  if (opts->retain)
  {
    first_byte |= 0x01;
  }

  az_result rc = _write_byte(dest, first_byte);
  if (az_result_failed(rc))
    return rc;
  rc = _write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;

  // Topic Name
  rc = _write_utf8_string(dest, opts->topic);
  if (az_result_failed(rc))
    return rc;

  // Packet Identifier (for QoS > 0)
  if (opts->qos != AZ_MQTT3_QOS_AT_MOST_ONCE)
  {
    rc = _write_uint16(dest, packet_id);
    if (az_result_failed(rc))
      return rc;
  }

  // Payload (raw, no length prefix)
  if (az_span_size(opts->payload) > 0)
  {
    if (az_span_size(*dest) < az_span_size(opts->payload))
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }
    memcpy(az_span_ptr(*dest), az_span_ptr(opts->payload), (size_t)az_span_size(opts->payload));
    *dest = az_span_slice_to_end(*dest, az_span_size(opts->payload));
  }

  return AZ_OK;
}

// ============================================================================
// Simple ACK encoding (PUBACK, PUBREC, PUBREL, PUBCOMP)
// ============================================================================

static az_result _encode_simple_ack(
    az_span* dest,
    az_mqtt3_packet_type type,
    uint8_t fixed_flags,
    uint16_t packet_id,
    az_mqtt3_reason_code reason_code)
{
  (void)reason_code;
  int32_t remaining = 2; // MQTT 3.1.1 ACK packets are packet-id only.

  az_result rc = _write_byte(dest, (uint8_t)((uint8_t)(type << 4) | fixed_flags));
  if (az_result_failed(rc))
    return rc;
  rc = _write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;
  rc = _write_uint16(dest, packet_id);
  if (az_result_failed(rc))
    return rc;

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt3_codec_encode_puback(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt3_reason_code reason_code)
{
  return _encode_simple_ack(dest, AZ_MQTT3_PACKET_TYPE_PUBACK, 0, packet_id, reason_code);
}

AZ_NODISCARD az_result az_mqtt3_codec_encode_pubrec(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt3_reason_code reason_code)
{
  return _encode_simple_ack(dest, AZ_MQTT3_PACKET_TYPE_PUBREC, 0, packet_id, reason_code);
}

AZ_NODISCARD az_result az_mqtt3_codec_encode_pubrel(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt3_reason_code reason_code)
{
  // PUBREL has fixed flags = 0x02
  return _encode_simple_ack(dest, AZ_MQTT3_PACKET_TYPE_PUBREL, 0x02, packet_id, reason_code);
}

AZ_NODISCARD az_result az_mqtt3_codec_encode_pubcomp(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt3_reason_code reason_code)
{
  return _encode_simple_ack(dest, AZ_MQTT3_PACKET_TYPE_PUBCOMP, 0, packet_id, reason_code);
}

// ============================================================================
// SUBSCRIBE encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_encode_subscribe(
    az_span* dest,
    az_mqtt3_subscription const* subs,
    int32_t sub_count,
    uint16_t packet_id)
{
  _az_PRECONDITION_NOT_NULL(dest);
  _az_PRECONDITION_NOT_NULL(subs);
  _az_PRECONDITION(sub_count > 0);

  // Calculate remaining length
  int32_t remaining = 2; // Packet ID

  for (int32_t i = 0; i < sub_count; i++)
  {
    remaining += 2 + az_span_size(subs[i].topic_filter) + 1; // string + options byte
  }

  // Fixed header: SUBSCRIBE type with reserved flags = 0x02
  az_result rc = _write_byte(dest, (uint8_t)((AZ_MQTT3_PACKET_TYPE_SUBSCRIBE << 4) | 0x02));
  if (az_result_failed(rc))
    return rc;
  rc = _write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;

  // Packet Identifier
  rc = _write_uint16(dest, packet_id);
  if (az_result_failed(rc))
    return rc;

  // Payload
  for (int32_t i = 0; i < sub_count; i++)
  {
    rc = _write_utf8_string(dest, subs[i].topic_filter);
    if (az_result_failed(rc))
      return rc;

    uint8_t options = (uint8_t)subs[i].qos;
    // MQTT 3.1.1 subscribe options only carry requested QoS (bits 0-1).

    rc = _write_byte(dest, options);
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// UNSUBSCRIBE encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_encode_unsubscribe(
    az_span* dest,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t packet_id)
{
  _az_PRECONDITION_NOT_NULL(dest);
  _az_PRECONDITION_NOT_NULL(topic_filters);
  _az_PRECONDITION(filter_count > 0);

  int32_t remaining = 2; // Packet ID

  for (int32_t i = 0; i < filter_count; i++)
  {
    remaining += 2 + az_span_size(topic_filters[i]);
  }

  // Fixed header: UNSUBSCRIBE type with reserved flags = 0x02
  az_result rc = _write_byte(dest, (uint8_t)((AZ_MQTT3_PACKET_TYPE_UNSUBSCRIBE << 4) | 0x02));
  if (az_result_failed(rc))
    return rc;
  rc = _write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;

  rc = _write_uint16(dest, packet_id);
  if (az_result_failed(rc))
    return rc;

  for (int32_t i = 0; i < filter_count; i++)
  {
    rc = _write_utf8_string(dest, topic_filters[i]);
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// PINGREQ encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_encode_pingreq(az_span* dest)
{
  _az_PRECONDITION_NOT_NULL(dest);

  az_result rc = _write_byte(dest, (uint8_t)(AZ_MQTT3_PACKET_TYPE_PINGREQ << 4));
  if (az_result_failed(rc))
    return rc;
  rc = _write_byte(dest, 0); // Remaining length = 0
  if (az_result_failed(rc))
    return rc;
  return AZ_OK;
}

// ============================================================================
// DISCONNECT encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_encode_disconnect(
    az_span* dest,
    az_mqtt3_reason_code reason_code,
    uint32_t session_expiry_interval)
{
  _az_PRECONDITION_NOT_NULL(dest);
  (void)reason_code;
  (void)session_expiry_interval;

  az_result rc = _write_byte(dest, (uint8_t)(AZ_MQTT3_PACKET_TYPE_DISCONNECT << 4));
  if (az_result_failed(rc))
    return rc;
  rc = _write_vbi(dest, 0);
  if (az_result_failed(rc))
    return rc;
  return AZ_OK;
}

// ============================================================================
// AUTH encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_encode_auth(
    az_span* dest,
    az_mqtt3_reason_code reason_code,
    az_span authentication_method,
    az_span authentication_data)
{
  _az_PRECONDITION_NOT_NULL(dest);

  int32_t prop_len = 0;
  if (az_span_size(authentication_method) > 0)
  {
    prop_len += 1 + 2 + az_span_size(authentication_method);
  }
  if (az_span_size(authentication_data) > 0)
  {
    prop_len += 1 + 2 + az_span_size(authentication_data);
  }

  int32_t remaining = 1 + _vbi_size(prop_len) + prop_len;

  az_result rc = _write_byte(dest, (uint8_t)(AZ_MQTT3_PACKET_TYPE_AUTH << 4));
  if (az_result_failed(rc))
    return rc;
  rc = _write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;

  rc = _write_byte(dest, (uint8_t)reason_code);
  if (az_result_failed(rc))
    return rc;

  rc = _write_vbi(dest, prop_len);
  if (az_result_failed(rc))
    return rc;

  if (az_span_size(authentication_method) > 0)
  {
    rc = _write_byte(dest, AZ_MQTT3_PROPERTY_AUTHENTICATION_METHOD);
    if (az_result_failed(rc))
      return rc;
    rc = _write_utf8_string(dest, authentication_method);
    if (az_result_failed(rc))
      return rc;
  }
  if (az_span_size(authentication_data) > 0)
  {
    rc = _write_byte(dest, AZ_MQTT3_PROPERTY_AUTHENTICATION_DATA);
    if (az_result_failed(rc))
      return rc;
    rc = _write_binary_data(dest, authentication_data);
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// Fixed header decoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_decode_fixed_header(
    az_span* src,
    az_mqtt3_packet_type* out_packet_type,
    uint8_t* out_flags,
    int32_t* out_remaining)
{
  _az_PRECONDITION_NOT_NULL(src);
  _az_PRECONDITION_NOT_NULL(out_packet_type);
  _az_PRECONDITION_NOT_NULL(out_flags);
  _az_PRECONDITION_NOT_NULL(out_remaining);

  uint8_t first_byte;
  az_result rc = _read_byte(src, &first_byte);
  if (az_result_failed(rc))
    return rc;

  *out_packet_type = (az_mqtt3_packet_type)(first_byte >> 4);
  *out_flags = first_byte & 0x0F;

  rc = _read_vbi(src, out_remaining);
  if (az_result_failed(rc))
    return rc;

  return AZ_OK;
}

// ============================================================================
// CONNACK decoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_decode_connack(az_span body, az_mqtt3_connack_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  // Save caller-provided buffers before clearing
  az_mqtt3_user_property* user_properties = out->user_properties;
  int32_t user_property_capacity = out->user_property_capacity;

  // Initialize defaults and preserve caller buffers.
  memset(out, 0, sizeof(*out));
  out->user_properties = user_properties;
  out->user_property_capacity = user_property_capacity;

  // Acknowledge Flags
  uint8_t ack_flags;
  az_result rc = _read_byte(&body, &ack_flags);
  if (az_result_failed(rc))
    return rc;
  out->session_present = (ack_flags & 0x01) != 0;

  // Return Code (MQTT 3.1.1)
  uint8_t return_code;
  rc = _read_byte(&body, &return_code);
  if (az_result_failed(rc))
    return rc;
  // Kept verbatim (AZ_MQTT3_CONNACK_*): "not authorized" must stay distinguishable
  // from "server unavailable".
  out->reason_code = (az_mqtt3_reason_code)return_code;

  if (az_span_size(body) != 0)
  {
    return AZ_MQTT3_ERROR_MALFORMED_PACKET;
  }

  return AZ_OK;
}

// ============================================================================
// PUBLISH decoding
// ============================================================================

AZ_NODISCARD az_result
az_mqtt3_codec_decode_publish(az_span body, uint8_t flags, az_mqtt3_publish_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  // Save caller-provided buffers before clearing
  az_mqtt3_user_property* user_properties = out->user_properties;
  int32_t* subscription_identifiers = out->subscription_identifiers;

  memset(out, 0, sizeof(*out));
  out->user_properties = user_properties;
  out->subscription_identifiers = subscription_identifiers;

  out->dup = (flags & 0x08) != 0;
  out->qos = (az_mqtt3_qos)((flags >> 1) & 0x03);
  out->retain = (flags & 0x01) != 0;

  // Topic Name
  az_result rc = _read_utf8_string(&body, &out->topic);
  if (az_result_failed(rc))
    return rc;

  // Packet Identifier (QoS > 0)
  if (out->qos != AZ_MQTT3_QOS_AT_MOST_ONCE)
  {
    rc = _read_uint16(&body, &out->packet_id);
    if (az_result_failed(rc))
      return rc;
  }

  // The rest is the payload
  out->payload = body;

  return AZ_OK;
}

// ============================================================================
// ACK decoding (PUBACK, PUBREC, PUBREL, PUBCOMP)
// ============================================================================

static az_result _decode_ack_props(az_span* src, az_mqtt3_ack_data* out)
{
  int32_t prop_len;
  az_result rc = _read_vbi(src, &prop_len);
  if (az_result_failed(rc))
    return rc;

  if (prop_len > az_span_size(*src))
  {
    return AZ_MQTT3_ERROR_MALFORMED_PACKET;
  }

  az_span props = az_span_slice(*src, 0, prop_len);
  *src = az_span_slice_to_end(*src, prop_len);

  while (az_span_size(props) > 0)
  {
    uint8_t prop_id;
    rc = _read_byte(&props, &prop_id);
    if (az_result_failed(rc))
      return rc;

    switch ((az_mqtt3_property_id)prop_id)
    {
      case AZ_MQTT3_PROPERTY_REASON_STRING:
        rc = _read_utf8_string(&props, &out->reason_string);
        break;
      case AZ_MQTT3_PROPERTY_USER_PROPERTY:
      {
        az_span key, value;
        rc = _read_utf8_string(&props, &key);
        if (az_result_failed(rc))
          return rc;
        rc = _read_utf8_string(&props, &value);
        if (az_result_failed(rc))
          return rc;
        if (out->user_properties != NULL
            && out->user_property_count < out->user_property_capacity)
        {
          out->user_properties[out->user_property_count].key = key;
          out->user_properties[out->user_property_count].value = value;
          out->user_property_count++;
        }
        continue;
      }
      default:
        return AZ_MQTT3_ERROR_MALFORMED_PACKET;
    }
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt3_codec_decode_ack(az_span body, az_mqtt3_ack_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  // Save caller-provided buffers before clearing
  az_mqtt3_user_property* user_properties = out->user_properties;
  int32_t user_property_capacity = out->user_property_capacity;

  memset(out, 0, sizeof(*out));
  out->user_properties = user_properties;
  out->user_property_capacity = user_property_capacity;

  az_result rc = _read_uint16(&body, &out->packet_id);
  if (az_result_failed(rc))
    return rc;

  // If remaining length was 2, no reason code / properties
  if (az_span_size(body) == 0)
  {
    out->reason_code = AZ_MQTT3_REASON_SUCCESS;
    return AZ_OK;
  }

  uint8_t reason;
  rc = _read_byte(&body, &reason);
  if (az_result_failed(rc))
    return rc;
  out->reason_code = (az_mqtt3_reason_code)reason;

  if (az_span_size(body) > 0)
  {
    rc = _decode_ack_props(&body, out);
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// SUBACK / UNSUBACK decoding
// ============================================================================

static az_result _decode_suback_common(az_span body, az_mqtt3_suback_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  // Save caller-provided buffers before clearing
  az_mqtt3_reason_code* reason_codes = out->reason_codes;
  int32_t reason_code_capacity = out->reason_code_capacity;
  az_mqtt3_user_property* user_properties = out->user_properties;
  int32_t user_property_capacity = out->user_property_capacity;

  memset(out, 0, sizeof(*out));
  out->reason_codes = reason_codes;
  out->reason_code_capacity = reason_code_capacity;
  out->user_properties = user_properties;
  out->user_property_capacity = user_property_capacity;

  az_result rc = _read_uint16(&body, &out->packet_id);
  if (az_result_failed(rc))
    return rc;

  // Remaining bytes are reason codes
  out->reason_code_count = 0;
  while (az_span_size(body) > 0)
  {
    uint8_t reason;
    rc = _read_byte(&body, &reason);
    if (az_result_failed(rc))
      return rc;
    // Like user properties: keep what fits, so reason_code_count never exceeds capacity.
    if (out->reason_codes != NULL && out->reason_code_count < out->reason_code_capacity)
    {
      out->reason_codes[out->reason_code_count] = (az_mqtt3_reason_code)reason;
      out->reason_code_count++;
    }
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt3_codec_decode_suback(az_span body, az_mqtt3_suback_data* out)
{
  return _decode_suback_common(body, out);
}

AZ_NODISCARD az_result az_mqtt3_codec_decode_unsuback(az_span body, az_mqtt3_suback_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  az_mqtt3_reason_code* reason_codes = out->reason_codes;
  int32_t reason_code_capacity = out->reason_code_capacity;
  az_mqtt3_user_property* user_properties = out->user_properties;
  int32_t user_property_capacity = out->user_property_capacity;

  memset(out, 0, sizeof(*out));
  out->reason_codes = reason_codes;
  out->reason_code_capacity = reason_code_capacity;
  out->user_properties = user_properties;
  out->user_property_capacity = user_property_capacity;

  az_result rc = _read_uint16(&body, &out->packet_id);
  if (az_result_failed(rc))
  {
    return rc;
  }

  if (az_span_size(body) != 0)
  {
    return AZ_MQTT3_ERROR_MALFORMED_PACKET;
  }

  return AZ_OK;
}

// ============================================================================
// DISCONNECT decoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_decode_disconnect(az_span body, az_mqtt3_disconnect_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  // Save caller-provided buffers before clearing
  az_mqtt3_user_property* user_properties = out->user_properties;
  int32_t user_property_capacity = out->user_property_capacity;

  memset(out, 0, sizeof(*out));
  out->user_properties = user_properties;
  out->user_property_capacity = user_property_capacity;

  // MQTT 3.1.1 DISCONNECT has no variable header or payload.
  if (az_span_size(body) != 0)
  {
    return AZ_MQTT3_ERROR_MALFORMED_PACKET;
  }

  out->reason_code = AZ_MQTT3_REASON_NORMAL_DISCONNECTION;
  return AZ_OK;
}

// ============================================================================
// AUTH decoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_decode_auth(az_span body, az_mqtt3_auth_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  // Save caller-provided buffers before clearing
  az_mqtt3_user_property* user_properties = out->user_properties;
  int32_t user_property_capacity = out->user_property_capacity;

  memset(out, 0, sizeof(*out));
  out->user_properties = user_properties;
  out->user_property_capacity = user_property_capacity;

  if (az_span_size(body) == 0)
  {
    out->reason_code = AZ_MQTT3_REASON_SUCCESS;
    return AZ_OK;
  }

  uint8_t reason;
  az_result rc = _read_byte(&body, &reason);
  if (az_result_failed(rc))
    return rc;
  out->reason_code = (az_mqtt3_reason_code)reason;

  if (az_span_size(body) == 0)
  {
    return AZ_OK;
  }

  int32_t prop_len;
  rc = _read_vbi(&body, &prop_len);
  if (az_result_failed(rc))
    return rc;

  if (prop_len > az_span_size(body))
  {
    return AZ_MQTT3_ERROR_MALFORMED_PACKET;
  }

  az_span props = az_span_slice(body, 0, prop_len);

  while (az_span_size(props) > 0)
  {
    uint8_t prop_id;
    rc = _read_byte(&props, &prop_id);
    if (az_result_failed(rc))
      return rc;

    switch ((az_mqtt3_property_id)prop_id)
    {
      case AZ_MQTT3_PROPERTY_AUTHENTICATION_METHOD:
        rc = _read_utf8_string(&props, &out->authentication_method);
        break;
      case AZ_MQTT3_PROPERTY_AUTHENTICATION_DATA:
        rc = _read_binary_data(&props, &out->authentication_data);
        break;
      case AZ_MQTT3_PROPERTY_REASON_STRING:
        rc = _read_utf8_string(&props, &out->reason_string);
        break;
      case AZ_MQTT3_PROPERTY_USER_PROPERTY:
      {
        az_span key, value;
        rc = _read_utf8_string(&props, &key);
        if (az_result_failed(rc))
          return rc;
        rc = _read_utf8_string(&props, &value);
        if (az_result_failed(rc))
          return rc;
        if (out->user_properties != NULL
            && out->user_property_count < out->user_property_capacity)
        {
          out->user_properties[out->user_property_count].key = key;
          out->user_properties[out->user_property_count].value = value;
          out->user_property_count++;
        }
        continue;
      }
      default:
        return AZ_MQTT3_ERROR_MALFORMED_PACKET;
    }
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}
