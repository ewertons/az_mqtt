// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_layers_internal.h
 * @brief Internal: the layers the platform transport is stacked from (TLS over HTTP CONNECT
 * proxy over socket; see az_mqtt_transport_init()), and what layers share.
 */

#ifndef AZ_MQTT_LAYERS_INTERNAL_H
#define AZ_MQTT_LAYERS_INTERNAL_H

#include "../platform/az_mqtt_http_connect.h"

#include <az_mqtt/az_mqtt_transport.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

// ──────────────────────── Native errors of a layer ───────────

/**
 * @brief Where a layer's native errors, and those of the transport below it, go.
 *
 * Errors from below are stamped with this layer's connect_attempt, and, while phase_result is
 * set (not AZ_OK), with that result: the one the layer returns for any failure below it then.
 */
typedef struct
{
  az_mqtt_transport_error_fn callback;
  void* context;
  uint32_t connect_attempt;
  az_result phase_result;
} _az_mqtt_layer_errors;

/** @brief Route @p lower's native errors through @p errors. */
void _az_mqtt_layer_errors_attach(_az_mqtt_layer_errors* errors, az_mqtt_transport* lower);

/** @brief Report one native error of the layer itself. */
void _az_mqtt_layer_report(
    _az_mqtt_layer_errors const* errors,
    az_mqtt_native_error_source source,
    int32_t code,
    az_result result);

/** @brief Milliseconds left until @p deadline_ms (-1: none), 0 if passed. */
int32_t _az_mqtt_layer_remaining(int64_t deadline_ms);

/** @brief Absolute deadline @p timeout_ms from now; -1 for a negative timeout. */
int64_t _az_mqtt_layer_deadline(int32_t timeout_ms);

// ──────────────────────── Socket (platform) ──────────────────

/** @brief Size of the socket transport (TCP; refuses TLS options and proxies). */
AZ_NODISCARD int32_t _az_mqtt_socket_transport_sizeof(void);

AZ_NODISCARD az_result _az_mqtt_socket_transport_init(az_mqtt_transport* transport);

// ──────────────────────── TLS (platform) ─────────────────────

/** @brief Size of the TLS layer; 0 in builds without a TLS backend. */
AZ_NODISCARD int32_t _az_mqtt_tls_transport_sizeof(void);

/**
 * @brief TLS over @p lower: connect_start()'s tls_options are this layer's (NULL: passes bytes
 * through); @p lower gets none.
 */
AZ_NODISCARD az_result
_az_mqtt_tls_transport_init(az_mqtt_transport* transport, az_mqtt_transport* lower);

// ──────────────────────── HTTP CONNECT proxy (portable) ──────

#ifndef AZ_MQTT_NO_PROXY
/** @brief HTTP CONNECT proxy layer (az_mqtt_transport_set_proxy()). Fields are internal. */
typedef struct
{
  az_mqtt_transport base;
  az_mqtt_transport* lower;
  _az_mqtt_layer_errors errors;
  /** @brief Set with set_proxy(); NULL: connect directly. */
  az_mqtt_proxy_options const* proxy;
  /** @brief The current connect's (kept until the next connect_start()). */
  az_mqtt_proxy_options const* active;
  az_span host;
  _az_mqtt_http_reply reply;
  uint16_t port;
  uint8_t stage;
  /** @brief Bytes after the CONNECT reply: the tunnelled stream's first. */
  uint16_t stash_start;
  uint16_t stash_end;
  uint8_t stash[64];
} _az_mqtt_proxy_transport;

AZ_NODISCARD az_result
_az_mqtt_proxy_transport_init(_az_mqtt_proxy_transport* proxy, az_mqtt_transport* lower);
#endif // AZ_MQTT_NO_PROXY

#endif // AZ_MQTT_LAYERS_INTERNAL_H
