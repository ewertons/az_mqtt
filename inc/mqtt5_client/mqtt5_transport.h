// Copyright (c) mqtt5_client contributors. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file mqtt5_transport.h
 * @brief Platform transport abstraction for TCP/TLS connections.
 *
 * Implement these functions for your platform. Implementations for Linux (POSIX + OpenSSL)
 * and Windows (Winsock + Schannel/OpenSSL) are provided.
 */

#ifndef MQTT5_TRANSPORT_H
#define MQTT5_TRANSPORT_H

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

// ──────────────────────── Transport handle ───────────────────

/**
 * @brief Opaque transport handle. Each platform provides its own struct definition.
 */
typedef struct mqtt5_transport mqtt5_transport;

// ──────────────────────── TLS options ────────────────────────

typedef struct
{
  /** @brief Path to CA certificate file (PEM). Set to AZ_SPAN_EMPTY to use system CAs. */
  az_span ca_cert_path;

  /** @brief Path to client certificate file (PEM) for mutual TLS. Optional. */
  az_span client_cert_path;

  /** @brief Path to client private key file (PEM) for mutual TLS. Optional. */
  az_span client_key_path;
} mqtt5_tls_options;

AZ_NODISCARD AZ_INLINE mqtt5_tls_options mqtt5_tls_options_default(void)
{
  mqtt5_tls_options opts;
  opts.ca_cert_path = AZ_SPAN_EMPTY;
  opts.client_cert_path = AZ_SPAN_EMPTY;
  opts.client_key_path = AZ_SPAN_EMPTY;
  return opts;
}

// ──────────────────────── Transport API ──────────────────────

/**
 * @brief Get the size in bytes of the platform-specific transport struct.
 * Use this to stack-allocate the transport: uint8_t buf[mqtt5_transport_sizeof()]; then cast.
 */
AZ_NODISCARD int32_t mqtt5_transport_sizeof(void);

/**
 * @brief Initialize a transport handle (already allocated by the caller).
 */
AZ_NODISCARD az_result mqtt5_transport_init(mqtt5_transport* transport);

/**
 * @brief Connect to a host over TCP, optionally with TLS.
 *
 * @param transport   Initialized transport handle.
 * @param host        Null-terminated hostname string (as az_span).
 * @param port        Destination port (e.g. 1883 or 8883).
 * @param tls_options TLS settings. Pass NULL for plain TCP.
 */
AZ_NODISCARD az_result mqtt5_transport_connect(
    mqtt5_transport* transport,
    az_span host,
    uint16_t port,
    mqtt5_tls_options const* tls_options);

/**
 * @brief Send bytes over the transport.
 * @return AZ_OK on success, or an error.
 */
AZ_NODISCARD az_result mqtt5_transport_send(mqtt5_transport* transport, az_span data);

/**
 * @brief Receive bytes from the transport.
 *
 * @param transport    Transport handle.
 * @param buffer       Buffer to read into.
 * @param timeout_ms   Timeout in milliseconds. 0 = non-blocking, -1 = block forever.
 * @param out_received Output: the sub-span of buffer that was filled.
 * @return AZ_OK on success (out_received size can be 0 on timeout), or an error.
 */
AZ_NODISCARD az_result mqtt5_transport_receive(
    mqtt5_transport* transport,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received);

/**
 * @brief Close the transport connection and release resources.
 */
void mqtt5_transport_close(mqtt5_transport* transport);

#include <azure/core/_az_cfg_suffix.h>

#endif // MQTT5_TRANSPORT_H
