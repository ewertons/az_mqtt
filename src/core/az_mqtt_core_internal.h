// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_core_internal.h
 * @brief Internal: session engine used by the az_mqttv3 and az_mqttv5 clients.
 *
 * The core owns the transport, packet framing, the receive buffer, keep-alive
 * and session state. The version client encodes and decodes packets and
 * reports them to the application.
 */

#ifndef AZ_MQTT_CORE_INTERNAL_H
#define AZ_MQTT_CORE_INTERNAL_H

#include <az_mqtt/az_mqtt_core.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdint.h>

/**
 * @brief Handle one received packet (body excludes the fixed header).
 *
 * @p core is the first member of the version client. May end the session
 * (_az_mqtt_core_close()); the core then stops using the receive buffer.
 */
typedef az_result (*_az_mqtt_core_dispatch_fn)(
    az_mqtt_core* core,
    az_mqtt_packet_type type,
    uint8_t flags,
    az_span body);

/**
 * @brief Start connecting: state CONNECTING, transport connect started.
 *
 * The CONNECT packet must already be encoded at the start of the send buffer;
 * it stays there until sent (nothing else uses the send buffer while
 * CONNECTING). _az_mqtt_core_process_loop() then completes the transport
 * connect, sends CONNECT and dispatches the CONNACK; its dispatch sets state
 * CONNECTED (and keep_alive_seconds) on acceptance. @p timeout_ms (-1: none)
 * bounds the whole sequence. The session is closed on failure.
 */
AZ_NODISCARD az_result _az_mqtt_core_connect_start(az_mqtt_core* core, int32_t timeout_ms);

/**
 * @brief Run the process loop until a started connect completes.
 *
 * @return AZ_OK once connected; AZ_MQTT_ERROR_NOT_CONNECTED if the CONNACK
 * refused or a callback ended the session; else the failure.
 */
AZ_NODISCARD az_result
_az_mqtt_core_connect_wait(az_mqtt_core* core, _az_mqtt_core_dispatch_fn dispatch);

/**
 * @brief Keep-alive, connect progress and dispatch of received packets; see
 * az_mqtt5_client_process_loop().
 */
AZ_NODISCARD az_result _az_mqtt_core_process_loop(
    az_mqtt_core* core,
    int32_t timeout_ms,
    _az_mqtt_core_dispatch_fn dispatch);

/**
 * @brief Send the send-buffer bytes an encoder wrote, @p remaining being what it left.
 *
 * Does not close on failure.
 * @retval AZ_MQTT_ERROR_PACKET_TOO_LARGE Over the server's Maximum Packet Size; nothing sent.
 */
AZ_NODISCARD az_result _az_mqtt_core_send(
    az_mqtt_core* core,
    az_span remaining);

/**
 * @brief End the session: close the transport, reset state and, if it was
 * connecting or connected, call on_closed with @p reason.
 */
void _az_mqtt_core_close(
    az_mqtt_core* core,
    az_result reason);

/**
 * @brief As _az_mqtt_core_send(), but closes the session if the send fails (a
 * partial packet may be on the wire).
 */
AZ_NODISCARD AZ_INLINE az_result _az_mqtt_core_send_request(az_mqtt_core* core, az_span remaining)
{
  az_result rc = _az_mqtt_core_send(core, remaining);
  if (az_result_failed(rc))
  {
    _az_mqtt_core_close(core, rc);
  }
  return rc;
}

/**
 * @brief Have the transport's native errors logged and passed to on_transport_error. Call once
 * the transport is set.
 */
void _az_mqtt_core_register_transport_errors(az_mqtt_core* core);

/** @brief What an in-flight entry holds. PUBLISH_QOS1 to PUBREL are what Receive Maximum counts. */
typedef enum
{
  _AZ_MQTT_INFLIGHT_FREE = 0,
  /** @brief PUBLISH QoS 1 sent; awaiting PUBACK. */
  _AZ_MQTT_INFLIGHT_PUBLISH_QOS1,
  /** @brief PUBLISH QoS 2 sent; awaiting PUBREC. */
  _AZ_MQTT_INFLIGHT_PUBLISH_QOS2,
  /** @brief PUBREL sent; awaiting PUBCOMP. */
  _AZ_MQTT_INFLIGHT_PUBREL,
  _AZ_MQTT_INFLIGHT_SUBSCRIBE,
  _AZ_MQTT_INFLIGHT_UNSUBSCRIBE,
  /** @brief PUBLISH QoS 2 received, PUBREC sent; awaiting PUBREL. Server's packet identifier. */
  _AZ_MQTT_INFLIGHT_INBOUND_QOS2,
} _az_mqtt_inflight_kind;

/** @brief Use @p buffer (az_mqtt_inflight_entry[]; at most UINT16_MAX used); all entries free. */
void _az_mqtt_core_inflight_init(az_mqtt_core* core, az_span buffer);

/**
 * @brief Reserve a free entry for an outgoing request, with a packet identifier no
 * other outgoing request holds.
 *
 * @param publish_limit Fail if this many outgoing QoS 1/2 PUBLISH exchanges are
 * incomplete (the server's Receive Maximum); UINT16_MAX for none.
 * @retval AZ_MQTT_ERROR_FLOW_CONTROL No free entry, or @p publish_limit reached.
 */
AZ_NODISCARD az_result _az_mqtt_core_inflight_reserve_entry(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t publish_limit,
    az_mqtt_inflight_entry** out_entry);

/** @brief The entry of @p kind holding @p packet_id, or NULL. */
AZ_NODISCARD az_mqtt_inflight_entry* _az_mqtt_core_inflight_find_entry(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t packet_id);

/** @brief Free the entry of @p kind holding @p packet_id; whether there was one. */
AZ_NODISCARD bool _az_mqtt_core_inflight_release_entry(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t packet_id);

/**
 * @brief Track an inbound QoS 2 PUBLISH until its PUBREL.
 *
 * @param[out] out_is_duplicate Whether @p packet_id already awaits PUBREL (do not
 * deliver it again). Without a free entry nothing is tracked, so a resent
 * duplicate would be delivered again.
 */
void _az_mqtt_core_inflight_track_inbound_qos2(
    az_mqtt_core* core,
    uint16_t packet_id,
    bool* out_is_duplicate);

/**
 * @brief Send a request encoded into the send buffer (@p encode_result: the
 * encoder's result; @p remaining: what it left of the buffer).
 *
 * On an encoding failure, or a packet over the server's Maximum Packet Size
 * (AZ_MQTT_ERROR_PACKET_TOO_LARGE), frees @p entry (may be NULL) and sends
 * nothing. A send failure closes the session, as _az_mqtt_core_send_request().
 */
AZ_NODISCARD az_result _az_mqtt_core_send_tracked_request(
    az_mqtt_core* core,
    az_mqtt_inflight_entry* entry,
    az_result encode_result,
    az_span remaining);

#endif // AZ_MQTT_CORE_INTERNAL_H
