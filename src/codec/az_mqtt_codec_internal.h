// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_codec_internal.h
 * @brief Internal: wire primitives shared by the MQTT 3.1.1 and 5.0 codecs.
 *
 * Defined here (static) so each codec compiles its own copy, as a single-version
 * build would with no sharing; nothing is linked across versions.
 *
 * Writers fail with AZ_ERROR_NOT_ENOUGH_SPACE (AZ_ERROR_ARG for a value the
 * format cannot hold); readers with AZ_ERROR_UNEXPECTED_END
 * (AZ_MQTT_ERROR_MALFORMED_PACKET for a Variable Byte Integer over 4 bytes).
 * A failed call may have consumed part of the span: abandon the packet.
 */

#ifndef AZ_MQTT_CODEC_INTERNAL_H
#define AZ_MQTT_CODEC_INTERNAL_H

#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdint.h>
#include <string.h>

/** @brief Storage for the helpers below: static, and not flagged when a codec leaves one unused. */
#if defined(__GNUC__) || defined(__clang__)
#define _AZ_MQTT_HELPER static __attribute__((unused))
#else
#define _AZ_MQTT_HELPER static inline
#endif

/** @brief Write one byte; advance @p dest. */
AZ_NODISCARD _AZ_MQTT_HELPER az_result _az_mqtt_write_byte(az_span* dest, uint8_t b)
{
  if (az_span_size(*dest) < 1)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  az_span_ptr(*dest)[0] = b;
  *dest = az_span_slice_to_end(*dest, 1);
  return AZ_OK;
}

/** @brief Write a big-endian 16-bit integer; advance @p dest. */
AZ_NODISCARD _AZ_MQTT_HELPER az_result _az_mqtt_write_uint16(az_span* dest, uint16_t val)
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

/** @brief Write a 2-byte length followed by @p str; advance @p dest. */
AZ_NODISCARD _AZ_MQTT_HELPER az_result _az_mqtt_write_utf8_string(az_span* dest, az_span str)
{
  int32_t len = az_span_size(str);
  if (len > 65535)
  {
    return AZ_ERROR_ARG;
  }

  az_result rc = _az_mqtt_write_uint16(dest, (uint16_t)len);
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

/** @brief Write a 2-byte length followed by @p data; advance @p dest. */
AZ_NODISCARD _AZ_MQTT_HELPER az_result _az_mqtt_write_binary_data(az_span* dest, az_span data)
{
  return _az_mqtt_write_utf8_string(dest, data);
}

/** @brief Write a Variable Byte Integer; advance @p dest. */
AZ_NODISCARD _AZ_MQTT_HELPER az_result _az_mqtt_write_vbi(az_span* dest, int32_t value)
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
    az_result rc = _az_mqtt_write_byte(dest, encoded_byte);
    if (az_result_failed(rc))
    {
      return rc;
    }
  } while (value > 0);
  return AZ_OK;
}

/** @brief Bytes a Variable Byte Integer of @p value takes (1-4). */
AZ_NODISCARD _AZ_MQTT_HELPER int32_t _az_mqtt_vbi_size(int32_t value)
{
  if (value <= 127)
    return 1;
  if (value <= 16383)
    return 2;
  if (value <= 2097151)
    return 3;
  return 4;
}

/** @brief Read one byte; advance @p src. */
AZ_NODISCARD _AZ_MQTT_HELPER az_result _az_mqtt_read_byte(az_span* src, uint8_t* out)
{
  if (az_span_size(*src) < 1)
  {
    return AZ_ERROR_UNEXPECTED_END;
  }
  *out = az_span_ptr(*src)[0];
  *src = az_span_slice_to_end(*src, 1);
  return AZ_OK;
}

/** @brief Read a big-endian 16-bit integer; advance @p src. */
AZ_NODISCARD _AZ_MQTT_HELPER az_result _az_mqtt_read_uint16(az_span* src, uint16_t* out)
{
  if (az_span_size(*src) < 2)
  {
    return AZ_ERROR_UNEXPECTED_END;
  }
  *out = (uint16_t)((uint16_t)az_span_ptr(*src)[0] << 8 | (uint16_t)az_span_ptr(*src)[1]);
  *src = az_span_slice_to_end(*src, 2);
  return AZ_OK;
}

/** @brief Read a Variable Byte Integer; advance @p src. */
AZ_NODISCARD _AZ_MQTT_HELPER az_result _az_mqtt_read_vbi(az_span* src, int32_t* out)
{
  int32_t result = 0;
  int shift = 0;
  for (int i = 0; i < 4; i++)
  {
    uint8_t b;
    az_result rc = _az_mqtt_read_byte(src, &b);
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
  return AZ_MQTT_ERROR_MALFORMED_PACKET;
}

/** @brief Read a length-prefixed string (a view into @p src); advance @p src. */
AZ_NODISCARD _AZ_MQTT_HELPER az_result _az_mqtt_read_utf8_string(az_span* src, az_span* out)
{
  uint16_t len;
  az_result rc = _az_mqtt_read_uint16(src, &len);
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

/** @brief Read length-prefixed binary data (a view into @p src); advance @p src. */
AZ_NODISCARD _AZ_MQTT_HELPER az_result _az_mqtt_read_binary_data(az_span* src, az_span* out)
{
  return _az_mqtt_read_utf8_string(src, out);
}

/** @brief Read a fixed header; advance @p src. */
AZ_NODISCARD _AZ_MQTT_HELPER az_result _az_mqtt_decode_fixed_header(
    az_span* src,
    az_mqtt_packet_type* out_packet_type,
    uint8_t* out_flags,
    int32_t* out_remaining)
{
  _az_PRECONDITION_NOT_NULL(src);
  _az_PRECONDITION_NOT_NULL(out_packet_type);
  _az_PRECONDITION_NOT_NULL(out_flags);
  _az_PRECONDITION_NOT_NULL(out_remaining);

  uint8_t first_byte;
  az_result rc = _az_mqtt_read_byte(src, &first_byte);
  if (az_result_failed(rc))
    return rc;

  *out_packet_type = (az_mqtt_packet_type)(first_byte >> 4);
  *out_flags = first_byte & 0x0F;

  rc = _az_mqtt_read_vbi(src, out_remaining);
  if (az_result_failed(rc))
    return rc;

  return AZ_OK;
}

/** @brief Write a PINGREQ (2 bytes); advance @p dest. */
AZ_NODISCARD _AZ_MQTT_HELPER az_result _az_mqtt_encode_pingreq(az_span* dest)
{
  _az_PRECONDITION_NOT_NULL(dest);

  az_result rc = _az_mqtt_write_byte(dest, (uint8_t)(AZ_MQTT_PACKET_TYPE_PINGREQ << 4));
  if (az_result_failed(rc))
    return rc;
  rc = _az_mqtt_write_byte(dest, 0); // Remaining length = 0
  if (az_result_failed(rc))
    return rc;
  return AZ_OK;
}

#endif // AZ_MQTT_CODEC_INTERNAL_H
