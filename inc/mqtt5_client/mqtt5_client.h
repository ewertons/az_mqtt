// Copyright (c) mqtt5_client contributors. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file mqtt5_client.h
 * @brief High-level MQTT 5.0 client API.
 *
 * Usage:
 *   1. Allocate mqtt5_client on the stack/static.
 *   2. Call mqtt5_client_init() with options and caller-owned buffers.
 *   3. Call mqtt5_client_connect() to establish the MQTT session.
 *   4. Use mqtt5_client_publish/subscribe/unsubscribe.
 *   5. Call mqtt5_client_process_loop() regularly to handle I/O and keepalive.
 *   6. Call mqtt5_client_disconnect() when done.
 *
 * Zero dynamic memory allocations. All buffers are provided by the caller.
 */

#ifndef MQTT5_CLIENT_H
#define MQTT5_CLIENT_H

#include <mqtt5_client/mqtt5_codec.h>
#include <mqtt5_client/mqtt5_transport.h>
#include <mqtt5_client/mqtt5_types.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <azure/core/_az_cfg_prefix.h>

// ──────────────────────── Callbacks ──────────────────────────

/** Forward declaration. */
typedef struct mqtt5_client mqtt5_client;

/**
 * @brief Called when a PUBLISH is received from the broker.
 */
typedef void (*mqtt5_on_publish_received_fn)(
    mqtt5_client* client,
    mqtt5_publish_data const* publish);

/**
 * @brief Called when a CONNACK is received.
 */
typedef void (*mqtt5_on_connack_fn)(mqtt5_client* client, mqtt5_connack_data const* connack);

/**
 * @brief Called when a SUBACK is received.
 */
typedef void (*mqtt5_on_suback_fn)(mqtt5_client* client, mqtt5_suback_data const* suback);

/**
 * @brief Called when an UNSUBACK is received.
 */
typedef void (*mqtt5_on_unsuback_fn)(mqtt5_client* client, mqtt5_suback_data const* unsuback);

/**
 * @brief Called when a PUBACK is received (QoS 1 acknowledgement).
 */
typedef void (*mqtt5_on_puback_fn)(mqtt5_client* client, mqtt5_ack_data const* ack);

/**
 * @brief Called when a PUBCOMP is received (QoS 2 complete).
 */
typedef void (*mqtt5_on_pubcomp_fn)(mqtt5_client* client, mqtt5_ack_data const* ack);

/**
 * @brief Called when the broker sends a DISCONNECT.
 */
typedef void (*mqtt5_on_disconnect_fn)(mqtt5_client* client, mqtt5_disconnect_data const* disc);

// ──────────────────────── Client options ─────────────────────

typedef struct
{
  /** @brief Transport handle (caller-allocated). */
  mqtt5_transport* transport;

  /** @brief Buffer for encoding outgoing packets. */
  az_span send_buffer;

  /** @brief Buffer for receiving incoming data. */
  az_span receive_buffer;

  /** @brief CONNECT options (client_id, keepalive, etc.). */
  mqtt5_connect_options connect_options;

  /** @brief TLS options (NULL for plain TCP). */
  mqtt5_tls_options const* tls_options;

  /** @brief Broker hostname. */
  az_span hostname;

  /** @brief Broker port. */
  uint16_t port;

  // Callbacks (all optional, set to NULL if not needed)
  mqtt5_on_connack_fn on_connack;
  mqtt5_on_publish_received_fn on_publish;
  mqtt5_on_suback_fn on_suback;
  mqtt5_on_unsuback_fn on_unsuback;
  mqtt5_on_puback_fn on_puback;
  mqtt5_on_pubcomp_fn on_pubcomp;
  mqtt5_on_disconnect_fn on_disconnect;

  /** @brief User context pointer (passthrough, not used by the library). */
  void* user_context;

  /** @brief Buffer for user properties in received CONNACK. */
  mqtt5_user_property* connack_user_properties;
  int32_t connack_user_property_capacity;

  /** @brief Buffer for user properties in received PUBLISH. */
  mqtt5_user_property* publish_user_properties;
  int32_t publish_user_property_capacity;

  /** @brief Buffer for subscription identifiers in received PUBLISH. */
  int32_t* publish_subscription_identifiers;
  int32_t publish_subscription_identifier_capacity;

  /** @brief Buffer for reason codes in SUBACK/UNSUBACK. */
  mqtt5_reason_code* suback_reason_codes;
  int32_t suback_reason_code_capacity;

  /** @brief Buffer for user properties in SUBACK/UNSUBACK. */
  mqtt5_user_property* suback_user_properties;
  int32_t suback_user_property_capacity;

  /** @brief Buffer for user properties in ACKs (PUBACK/PUBREC/PUBREL/PUBCOMP). */
  mqtt5_user_property* ack_user_properties;
  int32_t ack_user_property_capacity;

  /** @brief Buffer for user properties in received DISCONNECT. */
  mqtt5_user_property* disconnect_user_properties;
  int32_t disconnect_user_property_capacity;
} mqtt5_client_options;

// ──────────────────────── Client state ───────────────────────

typedef enum
{
  MQTT5_CLIENT_STATE_DISCONNECTED = 0,
  MQTT5_CLIENT_STATE_CONNECTING,
  MQTT5_CLIENT_STATE_CONNECTED,
} mqtt5_client_state;

struct mqtt5_client
{
  mqtt5_client_options options;
  mqtt5_client_state state;
  uint16_t next_packet_id;
  int64_t last_send_time_ms;
  int64_t last_receive_time_ms;

  // Receive buffer tracking
  int32_t recv_buf_pos; // how many bytes are buffered
};

// ──────────────────────── API ────────────────────────────────

/**
 * @brief Initialize the MQTT5 client.
 */
AZ_NODISCARD az_result mqtt5_client_init(mqtt5_client* client, mqtt5_client_options const* options);

/**
 * @brief Connect to the broker (TCP + optional TLS + MQTT CONNECT).
 * Blocks until CONNACK is received or timeout.
 */
AZ_NODISCARD az_result mqtt5_client_connect(mqtt5_client* client, int32_t timeout_ms);

/**
 * @brief Run the I/O processing loop once.
 * Reads incoming packets, dispatches callbacks, sends keepalive PINGs.
 *
 * @param timeout_ms  Max time to wait for incoming data.
 */
AZ_NODISCARD az_result mqtt5_client_process_loop(mqtt5_client* client, int32_t timeout_ms);

/**
 * @brief Publish a message.
 * @param[out] out_packet_id  Packet ID assigned (for QoS > 0). Can be NULL.
 */
AZ_NODISCARD az_result mqtt5_client_publish(
    mqtt5_client* client,
    mqtt5_publish_options const* options,
    uint16_t* out_packet_id);

/**
 * @brief Subscribe to topic(s).
 * @param[out] out_packet_id  Packet ID assigned. Can be NULL.
 */
AZ_NODISCARD az_result mqtt5_client_subscribe(
    mqtt5_client* client,
    mqtt5_subscription const* subscriptions,
    int32_t subscription_count,
    uint16_t* out_packet_id);

/**
 * @brief Unsubscribe from topic(s).
 * @param[out] out_packet_id  Packet ID assigned. Can be NULL.
 */
AZ_NODISCARD az_result mqtt5_client_unsubscribe(
    mqtt5_client* client,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t* out_packet_id);

/**
 * @brief Send a DISCONNECT and close the transport.
 */
AZ_NODISCARD az_result mqtt5_client_disconnect(
    mqtt5_client* client,
    mqtt5_reason_code reason_code);

/**
 * @brief Get current client state.
 */
AZ_NODISCARD AZ_INLINE mqtt5_client_state mqtt5_client_get_state(mqtt5_client const* client)
{
  return client->state;
}

/**
 * @brief Get the user context pointer.
 */
AZ_NODISCARD AZ_INLINE void* mqtt5_client_get_user_context(mqtt5_client const* client)
{
  return client->options.user_context;
}

#include <azure/core/_az_cfg_suffix.h>

#endif // MQTT5_CLIENT_H
