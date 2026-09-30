// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt3.h
 * @brief MQTT 3.1.1 codec: pass &az_mqtt3_codec to az_mqtt_client_init().
 *
 * All functions operate on caller-provided az_span buffers with zero dynamic allocation.
 */

#ifndef AZ_MQTT3_H
#define AZ_MQTT3_H

#include <az_mqtt/az_mqtt_codec.h>
#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <azure/core/_az_cfg_prefix.h>

/** @brief CONNECT Protocol Level this codec speaks (MQTT 3.1.1). */
#define AZ_MQTT3_PROTOCOL_VERSION 4

/** @brief The MQTT 3.1.1 codec. */
extern az_mqtt_codec const az_mqtt3_codec;

/**
 * @name MQTT 3.1.1 CONNACK return codes
 * @brief Stored verbatim in az_mqtt_connack_data::reason_code.
 * @{
 */
#define AZ_MQTT3_CONNACK_ACCEPTED 0x00
#define AZ_MQTT3_CONNACK_UNACCEPTABLE_PROTOCOL_VERSION 0x01
#define AZ_MQTT3_CONNACK_IDENTIFIER_REJECTED 0x02
#define AZ_MQTT3_CONNACK_SERVER_UNAVAILABLE 0x03
#define AZ_MQTT3_CONNACK_BAD_USER_NAME_OR_PASSWORD 0x04
#define AZ_MQTT3_CONNACK_NOT_AUTHORIZED 0x05
/** @} */

// ──────────────────────── Encoding ───────────────────────────

/**
 * @brief Encode a CONNECT packet into the destination buffer.
 * @param[in,out] dest  Buffer to write into. On success, advanced past the written bytes.
 * @param[in]     opts  CONNECT options.
 * @return AZ_OK or AZ_ERROR_NOT_ENOUGH_SPACE.
 */
AZ_NODISCARD az_result az_mqtt3_codec_encode_connect(az_span* dest, az_mqtt_connect_options const* opts);

/**
 * @brief Encode a PUBLISH packet.
 * @param[in,out] dest      Buffer; advanced on success.
 * @param[in]     opts      Publish options.
 * @param[in]     packet_id Packet identifier (must be non-zero for QoS>0).
 */
AZ_NODISCARD az_result az_mqtt3_codec_encode_publish(
    az_span* dest,
    az_mqtt_publish_options const* opts,
    uint16_t packet_id);

/**
 * @brief Encode a PUBACK packet.
 */
AZ_NODISCARD az_result az_mqtt3_codec_encode_puback(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt_reason_code reason_code);

/**
 * @brief Encode a PUBREC packet.
 */
AZ_NODISCARD az_result az_mqtt3_codec_encode_pubrec(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt_reason_code reason_code);

/**
 * @brief Encode a PUBREL packet.
 */
AZ_NODISCARD az_result az_mqtt3_codec_encode_pubrel(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt_reason_code reason_code);

/**
 * @brief Encode a PUBCOMP packet.
 */
AZ_NODISCARD az_result az_mqtt3_codec_encode_pubcomp(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt_reason_code reason_code);

/**
 * @brief Encode a SUBSCRIBE packet.
 * @param[in]     subs      Array of subscriptions.
 * @param[in]     sub_count Number of subscriptions.
 * @param[in]     packet_id Packet identifier.
 */
AZ_NODISCARD az_result az_mqtt3_codec_encode_subscribe(
    az_span* dest,
    az_mqtt_subscription const* subs,
    int32_t sub_count,
    uint16_t packet_id);

/**
 * @brief Encode an UNSUBSCRIBE packet.
 */
AZ_NODISCARD az_result az_mqtt3_codec_encode_unsubscribe(
    az_span* dest,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t packet_id);

/**
 * @brief Encode a DISCONNECT packet.
 */
AZ_NODISCARD az_result az_mqtt3_codec_encode_disconnect(
    az_span* dest,
    az_mqtt_reason_code reason_code,
    uint32_t session_expiry_interval);

// ──────────────────────── Decoding ───────────────────────────

/**
 * @brief Decode a CONNACK packet body (after fixed header).
 */
AZ_NODISCARD az_result az_mqtt3_codec_decode_connack(az_span body, az_mqtt_connack_data* out);

/**
 * @brief Decode a PUBLISH packet body (after fixed header).
 * @param[in] flags  Fixed header flags.
 */
AZ_NODISCARD az_result
az_mqtt3_codec_decode_publish(az_span body, uint8_t flags, az_mqtt_publish_data* out);

/**
 * @brief Decode PUBACK/PUBREC/PUBREL/PUBCOMP body.
 */
AZ_NODISCARD az_result az_mqtt3_codec_decode_ack(az_span body, az_mqtt_ack_data* out);

/**
 * @brief Decode SUBACK body.
 */
AZ_NODISCARD az_result az_mqtt3_codec_decode_suback(az_span body, az_mqtt_suback_data* out);

/**
 * @brief Decode UNSUBACK body.
 */
AZ_NODISCARD az_result az_mqtt3_codec_decode_unsuback(az_span body, az_mqtt_suback_data* out);

/**
 * @brief Decode a DISCONNECT packet body.
 */
AZ_NODISCARD az_result az_mqtt3_codec_decode_disconnect(az_span body, az_mqtt_disconnect_data* out);

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT3_H
