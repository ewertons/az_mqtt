// Copyright (c) az_mqtt5_client contributors. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file az_mqtt5_client.h
 * @brief High-level MQTT 5.0 client API.
 *
 * Usage:
 *   1. Allocate az_mqtt5_client on the stack/static.
 *   2. Call az_mqtt5_client_init() with options and caller-owned buffers.
 *   3. Call az_mqtt5_client_connect() to establish the MQTT session.
 *   4. Use az_mqtt5_client_publish/subscribe/unsubscribe.
 *   5. Call az_mqtt5_client_process_loop() regularly to handle I/O and keepalive.
 *   6. Call az_mqtt5_client_disconnect() when done.
 *
 * Zero dynamic memory allocations. All buffers are provided by the caller.
 */

#ifndef AZ_MQTT5_CLIENT_H
#define AZ_MQTT5_CLIENT_H

#include <az_mqtt5/az_mqtt5_codec.h>
#include <az_mqtt5/az_mqtt5_transport.h>
#include <az_mqtt5/az_mqtt5_types.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <azure/core/_az_cfg_prefix.h>

// ──────────────────────── Callbacks ──────────────────────────

/** Forward declaration. */
typedef struct az_mqtt5_client az_mqtt5_client;

/**
 * @brief Called when a PUBLISH is received from the broker.
 */
typedef void (*az_mqtt5_on_publish_received_fn)(
    az_mqtt5_client* client,
    az_mqtt5_publish_data const* publish);

/**
 * @brief Called when a CONNACK is received.
 */
typedef void (*az_mqtt5_on_connack_fn)(az_mqtt5_client* client, az_mqtt5_connack_data const* connack);

/**
 * @brief Called when a SUBACK is received.
 */
typedef void (*az_mqtt5_on_suback_fn)(az_mqtt5_client* client, az_mqtt5_suback_data const* suback);

/**
 * @brief Called when an UNSUBACK is received.
 */
typedef void (*az_mqtt5_on_unsuback_fn)(az_mqtt5_client* client, az_mqtt5_suback_data const* unsuback);

/**
 * @brief Called when a PUBACK is received (QoS 1 acknowledgement).
 */
typedef void (*az_mqtt5_on_puback_fn)(az_mqtt5_client* client, az_mqtt5_ack_data const* ack);

/**
 * @brief Called when a PUBCOMP is received (QoS 2 complete).
 */
typedef void (*az_mqtt5_on_pubcomp_fn)(az_mqtt5_client* client, az_mqtt5_ack_data const* ack);

/**
 * @brief Called when the broker sends a DISCONNECT.
 */
typedef void (*az_mqtt5_on_disconnect_fn)(az_mqtt5_client* client, az_mqtt5_disconnect_data const* disc);

// ──────────────────────── Client options ─────────────────────

typedef struct
{
  /** @brief Buffer for user properties in received CONNACK (az_mqtt5_user_property[]). */
  az_span connack_user_properties;

  /** @brief Buffer for user properties in received PUBLISH (az_mqtt5_user_property[]). */
  az_span publish_user_properties;

  /** @brief Buffer for subscription identifiers in received PUBLISH (int32_t[]). */
  az_span publish_subscription_identifiers;

  /** @brief Buffer for reason codes in SUBACK/UNSUBACK (az_mqtt5_reason_code[]). */
  az_span suback_reason_codes;

  /** @brief Buffer for user properties in SUBACK/UNSUBACK (az_mqtt5_user_property[]). */
  az_span suback_user_properties;

  /** @brief Buffer for user properties in ACKs (PUBACK/PUBREC/PUBREL/PUBCOMP) (az_mqtt5_user_property[]). */
  az_span ack_user_properties;

  /** @brief Buffer for user properties in received DISCONNECT (az_mqtt5_user_property[]). */
  az_span disconnect_user_properties;
} az_mqtt5_client_buffers;

typedef struct
{
  /** @brief Transport handle (caller-allocated). */
  az_mqtt5_transport* transport;

  /** @brief Buffer for encoding outgoing packets. */
  az_span send_buffer;

  /** @brief Buffer for receiving incoming data. */
  az_span receive_buffer;

  /** @brief CONNECT options (client_id, keepalive, etc.). */
  az_mqtt5_connect_options connect_options;

  /** @brief TLS options (NULL for plain TCP). */
  az_mqtt5_tls_options const* tls_options;

  /** @brief Broker hostname. */
  az_span hostname;

  /** @brief Broker port. */
  uint16_t port;

  // Callbacks (all optional, set to NULL if not needed)
  az_mqtt5_on_connack_fn on_connack;
  az_mqtt5_on_publish_received_fn on_publish;
  az_mqtt5_on_suback_fn on_suback;
  az_mqtt5_on_unsuback_fn on_unsuback;
  az_mqtt5_on_puback_fn on_puback;
  az_mqtt5_on_pubcomp_fn on_pubcomp;
  az_mqtt5_on_disconnect_fn on_disconnect;

  /** @brief User context pointer (passthrough, not used by the library). */
  void* user_context;

  /** @brief Caller-provided decode buffers used by callbacks and packet parsing. */
  az_mqtt5_client_buffers buffers;
} az_mqtt5_client_options;

// ──────────────────────── Client state ───────────────────────

typedef enum
{
  AZ_MQTT5_CLIENT_STATE_DISCONNECTED = 0,
  AZ_MQTT5_CLIENT_STATE_CONNECTING,
  AZ_MQTT5_CLIENT_STATE_CONNECTED,
} az_mqtt5_client_state;

struct az_mqtt5_client
{
  az_mqtt5_client_options options;
  az_mqtt5_client_state state;
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
AZ_NODISCARD az_result az_mqtt5_client_init(az_mqtt5_client* client, az_mqtt5_client_options const* options);

/**
 * @brief Connect to the broker (TCP + optional TLS + MQTT CONNECT).
 * Blocks until CONNACK is received or timeout.
 */
AZ_NODISCARD az_result az_mqtt5_client_connect(az_mqtt5_client* client, int32_t timeout_ms);

/**
 * @brief Run the I/O processing loop once.
 * Reads incoming packets, dispatches callbacks, sends keepalive PINGs.
 *
 * @param timeout_ms  Max time to wait for incoming data.
 */
AZ_NODISCARD az_result az_mqtt5_client_process_loop(az_mqtt5_client* client, int32_t timeout_ms);

/**
 * @brief Publish a message.
 * @param[out] out_packet_id  Packet ID assigned (for QoS > 0). Can be NULL.
 */
AZ_NODISCARD az_result az_mqtt5_client_publish(
    az_mqtt5_client* client,
    az_mqtt5_publish_options const* options,
    uint16_t* out_packet_id);

/**
 * @brief Subscribe to topic(s).
 * @param[out] out_packet_id  Packet ID assigned. Can be NULL.
 */
AZ_NODISCARD az_result az_mqtt5_client_subscribe(
    az_mqtt5_client* client,
    az_mqtt5_subscription const* subscriptions,
    int32_t subscription_count,
    uint16_t* out_packet_id);

/**
 * @brief Unsubscribe from topic(s).
 * @param[out] out_packet_id  Packet ID assigned. Can be NULL.
 */
AZ_NODISCARD az_result az_mqtt5_client_unsubscribe(
    az_mqtt5_client* client,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t* out_packet_id);

/**
 * @brief Send a DISCONNECT and close the transport.
 */
AZ_NODISCARD az_result az_mqtt5_client_disconnect(
    az_mqtt5_client* client,
    az_mqtt5_reason_code reason_code);

/**
 * @brief Get current client state.
 */
AZ_NODISCARD AZ_INLINE az_mqtt5_client_state az_mqtt5_client_get_state(az_mqtt5_client const* client)
{
  return client->state;
}

/**
 * @brief Get the user context pointer.
 */
AZ_NODISCARD AZ_INLINE void* az_mqtt5_client_get_user_context(az_mqtt5_client const* client)
{
  return client->options.user_context;
}

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT5_CLIENT_H
