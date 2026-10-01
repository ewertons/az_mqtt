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

/** @brief What an in-flight slot holds. PUBLISH_QOS1 to PUBREL are what Receive Maximum counts. */
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

/** @brief Use @p storage (az_mqtt_inflight[]) as the in-flight table; all slots free. */
void _az_mqtt_core_inflight_init(az_mqtt_core* core, az_span storage);

/**
 * @brief Take a free slot for an outgoing request, with a packet identifier no
 * other outgoing request holds.
 *
 * @param publish_limit Fail if this many outgoing QoS 1/2 PUBLISH exchanges are
 * incomplete (the server's Receive Maximum); UINT16_MAX for none.
 * @retval AZ_MQTT_ERROR_FLOW_CONTROL No free slot, or @p publish_limit reached.
 */
AZ_NODISCARD az_result _az_mqtt_core_inflight_reserve(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t publish_limit,
    az_mqtt_inflight** out_slot);

/** @brief The slot of @p kind holding @p packet_id, or NULL. */
az_mqtt_inflight*
_az_mqtt_core_inflight_find(az_mqtt_core* core, _az_mqtt_inflight_kind kind, uint16_t packet_id);

/** @brief Free the slot of @p kind holding @p packet_id; whether there was one. */
bool _az_mqtt_core_inflight_complete(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t packet_id);

/**
 * @brief Record an inbound QoS 2 PUBLISH until its PUBREL.
 *
 * @return true if @p packet_id is already awaiting PUBREL (a duplicate: do not
 * deliver it again). Without a free slot nothing is recorded and a resent
 * duplicate would be delivered again.
 */
bool _az_mqtt_core_inflight_inbound_qos2(az_mqtt_core* core, uint16_t packet_id);

/**
 * @brief Send a request the client encoded into the send buffer (@p encoded:
 * the encoder's result, @p remaining: what it left of the buffer).
 *
 * On an encoding failure, or a packet over @p maximum_packet_size (0: no
 * limit; AZ_MQTT_ERROR_PACKET_TOO_LARGE), frees @p slot (may be NULL) and sends
 * nothing. A send failure closes the session, as _az_mqtt_core_send_request().
 */
AZ_NODISCARD az_result _az_mqtt_core_send_tracked(
    az_mqtt_core* core,
    az_mqtt_inflight* slot,
    az_result encoded,
    az_span remaining,
    uint32_t maximum_packet_size);

#endif // AZ_MQTT_CORE_INTERNAL_H
