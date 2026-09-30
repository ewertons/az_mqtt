// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_codec.h
 * @brief Protocol-version seam between the client and a codec.
 *
 * The client is version-neutral and reaches the wire format only through an
 * az_mqtt_codec, chosen at az_mqtt_client_init(): &az_mqtt3_codec
 * (az_mqtt/az_mqtt3.h) or &az_mqtt5_codec (az_mqtt/az_mqtt5.h). Each codec is a
 * separate library, so an application that references one never links the other.
 */

#ifndef AZ_MQTT_CODEC_H
#define AZ_MQTT_CODEC_H

#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/**
 * @brief One MQTT protocol version's packet encoders and decoders.
 *
 * Every function writes to / reads from caller-provided spans and never allocates.
 * Encoders advance @p dest past what they wrote; decoders take the packet body
 * (after the fixed header). Fields a version does not have are ignored on
 * encode and left zero/empty on decode.
 */
typedef struct
{
  /** @brief CONNECT Protocol Level: 4 (MQTT 3.1.1) or 5 (MQTT 5.0). */
  uint8_t protocol_version;

  az_result (*encode_connect)(az_span* dest, az_mqtt_connect_options const* opts);
  az_result (*encode_publish)(
      az_span* dest,
      az_mqtt_publish_options const* opts,
      uint16_t packet_id);
  az_result (*encode_puback)(az_span* dest, uint16_t packet_id, az_mqtt_reason_code reason_code);
  az_result (*encode_pubrec)(az_span* dest, uint16_t packet_id, az_mqtt_reason_code reason_code);
  az_result (*encode_pubrel)(az_span* dest, uint16_t packet_id, az_mqtt_reason_code reason_code);
  az_result (*encode_pubcomp)(az_span* dest, uint16_t packet_id, az_mqtt_reason_code reason_code);
  az_result (*encode_subscribe)(
      az_span* dest,
      az_mqtt_subscription const* subs,
      int32_t sub_count,
      uint16_t packet_id);
  az_result (*encode_unsubscribe)(
      az_span* dest,
      az_span const* topic_filters,
      int32_t filter_count,
      uint16_t packet_id);
  az_result (*encode_disconnect)(
      az_span* dest,
      az_mqtt_reason_code reason_code,
      uint32_t session_expiry_interval);

  az_result (*decode_connack)(az_span body, az_mqtt_connack_data* out);
  az_result (*decode_publish)(az_span body, uint8_t flags, az_mqtt_publish_data* out);
  /** @brief PUBACK / PUBREC / PUBREL / PUBCOMP. */
  az_result (*decode_ack)(az_span body, az_mqtt_ack_data* out);
  az_result (*decode_suback)(az_span body, az_mqtt_suback_data* out);
  az_result (*decode_unsuback)(az_span body, az_mqtt_suback_data* out);
  /** @brief Server DISCONNECT. MQTT 3.1.1 defines none; its codec accepts an empty one. */
  az_result (*decode_disconnect)(az_span body, az_mqtt_disconnect_data* out);
} az_mqtt_codec;

// ──────────────────────── Version-neutral packets ────────────

/**
 * @brief Read a fixed header.
 *
 * @param[in,out] src             Source buffer; advanced past the header on success.
 * @param[out]    out_packet_type Packet type.
 * @param[out]    out_flags       Fixed header flags (lower nibble of first byte).
 * @param[out]    out_remaining   Remaining length value.
 * @return AZ_OK, AZ_ERROR_UNEXPECTED_END, or AZ_MQTT_ERROR_MALFORMED_PACKET.
 */
AZ_NODISCARD az_result az_mqtt_codec_decode_fixed_header(
    az_span* src,
    az_mqtt_packet_type* out_packet_type,
    uint8_t* out_flags,
    int32_t* out_remaining);

/**
 * @brief Encode a PINGREQ packet (2 bytes).
 */
AZ_NODISCARD az_result az_mqtt_codec_encode_pingreq(az_span* dest);

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT_CODEC_H
