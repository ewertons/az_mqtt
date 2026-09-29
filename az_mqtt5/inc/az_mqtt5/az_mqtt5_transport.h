// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt5_transport.h
 * @brief Platform transport abstraction for TCP/TLS connections.
 *
 * Implement these functions for your platform. Implementations for Linux (POSIX + OpenSSL)
 * and Windows (Winsock + Schannel/OpenSSL) are provided.
 */

#ifndef AZ_MQTT5_TRANSPORT_H
#define AZ_MQTT5_TRANSPORT_H

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

// ──────────────────────── Transport handle ───────────────────

/**
 * @brief Opaque transport handle. Each platform provides its own struct definition.
 */
typedef struct az_mqtt5_transport az_mqtt5_transport;

// ──────────────────────── TLS options ────────────────────────

/**
 * @brief TLS settings. The server certificate chain and host name (or IP
 * address) are always verified; there is no option to turn that off.
 */
typedef struct
{
  /**
   * @brief Path to CA certificate file (PEM). AZ_SPAN_EMPTY uses the system
   * store (OpenSSL, Schannel); mbedTLS has none and fails with
   * AZ_MQTT5_ERROR_NOT_SUPPORTED.
   */
  az_span ca_cert_path;

  /** @brief Client certificate file (PEM) for mutual TLS. Set with client_key_path or not at all. */
  az_span client_cert_path;

  /** @brief Client private key file (PEM) for mutual TLS. Set with client_cert_path or not at all. */
  az_span client_key_path;
} az_mqtt5_tls_options;

AZ_NODISCARD AZ_INLINE az_mqtt5_tls_options az_mqtt5_tls_options_default(void)
{
  az_mqtt5_tls_options opts;
  opts.ca_cert_path = AZ_SPAN_EMPTY;
  opts.client_cert_path = AZ_SPAN_EMPTY;
  opts.client_key_path = AZ_SPAN_EMPTY;
  return opts;
}

// ──────────────────────── Limits ─────────────────────────────

#ifndef AZ_MQTT5_TRANSPORT_CONNECT_TIMEOUT_MS
/** @brief Bound of az_mqtt5_transport_connect() (TCP connect + TLS handshake). */
#define AZ_MQTT5_TRANSPORT_CONNECT_TIMEOUT_MS 30000
#endif

#ifndef AZ_MQTT5_TRANSPORT_SEND_TIMEOUT_MS
/**
 * @brief Longest az_mqtt5_transport_send() waits for the peer to accept data.
 * On expiry the connection is unusable (a partial packet may have been sent).
 */
#define AZ_MQTT5_TRANSPORT_SEND_TIMEOUT_MS 30000
#endif

// ──────────────────────── Transport API ──────────────────────

/**
 * @brief Get the size in bytes of the platform-specific transport struct.
 * Use this to stack-allocate the transport: uint8_t buf[az_mqtt5_transport_sizeof()]; then cast.
 */
AZ_NODISCARD int32_t az_mqtt5_transport_sizeof(void);

/**
 * @brief Initialize a transport handle (already allocated by the caller).
 */
AZ_NODISCARD az_result az_mqtt5_transport_init(az_mqtt5_transport* transport);

/**
 * @brief Connect to a host over TCP, optionally with TLS.
 *
 * @param transport   Initialized transport handle.
 * @param host        Null-terminated hostname string (as az_span).
 * @param port        Destination port (e.g. 1883 or 8883).
 * @param tls_options TLS settings. Pass NULL for plain TCP.
 *
 * Bounded by AZ_MQTT5_TRANSPORT_CONNECT_TIMEOUT_MS (AZ_MQTT5_ERROR_TIMEOUT).
 *
 * @retval AZ_MQTT5_ERROR_NOT_SUPPORTED TLS requested from a build without a TLS
 *         backend, or an option the backend cannot honour. Never downgrades.
 * @retval AZ_MQTT5_ERROR_INVALID_CONFIG Only one of client_cert_path / client_key_path set.
 */
AZ_NODISCARD az_result az_mqtt5_transport_connect(
    az_mqtt5_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt5_tls_options const* tls_options);

/**
 * @brief Start a connect without waiting for it: resolves @p host and begins
 * the TCP connect. Drive it with az_mqtt5_transport_connect_poll().
 *
 * Name resolution is the only step that may block. Windows (Schannel) completes
 * the whole connect here and az_mqtt5_transport_connect_poll() returns at once.
 *
 * @param tls_options Same as az_mqtt5_transport_connect(); read only during this call.
 */
AZ_NODISCARD az_result az_mqtt5_transport_connect_start(
    az_mqtt5_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt5_tls_options const* tls_options);

/**
 * @brief Progress a started connect (TCP, then TLS handshake) for up to @p timeout_ms.
 *
 * @retval AZ_OK Connected.
 * @retval AZ_MQTT5_ERROR_TIMEOUT Not done yet; call again.
 * @retval other Failed; the transport is closed.
 */
AZ_NODISCARD az_result
az_mqtt5_transport_connect_poll(az_mqtt5_transport* transport, int32_t timeout_ms);

/**
 * @brief Send bytes over the transport.
 * @return AZ_OK on success, or an error.
 */
AZ_NODISCARD az_result az_mqtt5_transport_send(az_mqtt5_transport* transport, az_span data);

/**
 * @brief Receive bytes from the transport.
 *
 * @param transport    Transport handle.
 * @param buffer       Buffer to read into.
 * @param timeout_ms   Timeout in milliseconds. 0 = non-blocking, -1 = block forever.
 * @param out_received Output: the sub-span of buffer that was filled.
 * @return AZ_OK on success (out_received size can be 0 on timeout), or an error.
 */
AZ_NODISCARD az_result az_mqtt5_transport_receive(
    az_mqtt5_transport* transport,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received);

/**
 * @brief Close the transport connection and release resources.
 */
void az_mqtt5_transport_close(az_mqtt5_transport* transport);

/**
 * @brief Monotonic clock in milliseconds, used for keep-alive and timeouts.
 *
 * Part of the platform port, like the functions above: it must never go
 * backwards (not wall-clock time) and must have millisecond resolution.
 */
AZ_NODISCARD int64_t az_mqtt5_transport_clock_ms(void);

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT5_TRANSPORT_H
