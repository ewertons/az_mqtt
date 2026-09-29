// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file transport_mbedtls.c
 * @brief POSIX sockets transport, with TLS through mbedTLS when AZ_MQTT5_TLS_MBEDTLS is defined.
 */

#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE // inet_pton, ssize_t, recv under -std=c99
#endif

#include "az_mqtt5_socket_posix.h"

#include <az_mqtt5/az_mqtt5_transport.h>
#include <az_mqtt5/az_mqtt5_types.h>

#include <azure/core/az_span.h>
#include <azure/core/internal/az_precondition_internal.h>

#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef AZ_MQTT5_TLS_MBEDTLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#endif

// ──────────────────────── Platform-specific transport ────────

/** @brief Connection progress. */
typedef enum
{
  _TRANSPORT_IDLE = 0,
  _TRANSPORT_TCP,
  _TRANSPORT_TLS,
  _TRANSPORT_CONNECTED,
} _transport_state;

struct az_mqtt5_transport
{
  int socket_fd;
  _az_mqtt5_tcp_connect tcp;
  _transport_state state;
#ifdef AZ_MQTT5_TLS_MBEDTLS
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_x509_crt ca_chain;
  mbedtls_x509_crt client_cert;
  mbedtls_pk_context client_key;
  bool use_tls;
#endif
  bool connected;
};

#ifdef AZ_MQTT5_TLS_MBEDTLS
/** @brief Put every TLS context in its freshly-initialized state. */
static void _tls_contexts_init(az_mqtt5_transport* transport)
{
  mbedtls_ssl_init(&transport->ssl);
  mbedtls_ssl_config_init(&transport->conf);
  mbedtls_entropy_init(&transport->entropy);
  mbedtls_ctr_drbg_init(&transport->ctr_drbg);
  mbedtls_x509_crt_init(&transport->ca_chain);
  mbedtls_x509_crt_init(&transport->client_cert);
  mbedtls_pk_init(&transport->client_key);
  transport->use_tls = false;
}

/** @brief Release every TLS context and re-initialize it for the next connect. */
static void _tls_contexts_reset(az_mqtt5_transport* transport)
{
  mbedtls_ssl_free(&transport->ssl);
  mbedtls_ssl_config_free(&transport->conf);
  mbedtls_x509_crt_free(&transport->ca_chain);
  mbedtls_x509_crt_free(&transport->client_cert);
  mbedtls_pk_free(&transport->client_key);
  mbedtls_ctr_drbg_free(&transport->ctr_drbg);
  mbedtls_entropy_free(&transport->entropy);
  _tls_contexts_init(transport);
}
#endif

AZ_NODISCARD int32_t az_mqtt5_transport_sizeof(void) { return (int32_t)sizeof(az_mqtt5_transport); }

AZ_NODISCARD az_result az_mqtt5_transport_init(az_mqtt5_transport* transport)
{
  _az_PRECONDITION_NOT_NULL(transport);
  memset(transport, 0, sizeof(*transport));
  transport->socket_fd = -1;
  _az_mqtt5_tcp_connect_init(&transport->tcp);
  transport->state = _TRANSPORT_IDLE;
#ifdef AZ_MQTT5_TLS_MBEDTLS
  _tls_contexts_init(transport);
#endif
  return AZ_OK;
}

// Helper: copy az_span to null-terminated char buffer on the stack.
static az_result _span_to_cstr(az_span src, char* buf, int32_t buf_size)
{
  int32_t len = az_span_size(src);
  if (len >= buf_size)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  memcpy(buf, az_span_ptr(src), (size_t)len);
  buf[len] = '\0';
  return AZ_OK;
}

#ifdef AZ_MQTT5_TLS_MBEDTLS
// ──────────────────────── mbedTLS I/O callbacks ──────────────
//
// mbedtls_net_send() writes with write(), which raises SIGPIPE when the peer has
// reset the connection; these use send(MSG_NOSIGNAL) on the non-blocking socket.

static int _tls_send(void* ctx, unsigned char const* data, size_t size)
{
  int32_t const chunk = size > (size_t)INT32_MAX ? INT32_MAX : (int32_t)size;
  int32_t n = _az_mqtt5_send_nosignal(*(int*)ctx, data, chunk);
  if (n > 0)
  {
    return (int)n;
  }
  return n == 0 ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int _tls_recv(void* ctx, unsigned char* buffer, size_t size)
{
  ssize_t n = recv(*(int*)ctx, buffer, size, 0);
  if (n >= 0)
  {
    return (int)n; // 0 is an orderly close.
  }
  return _az_mqtt5_would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

// ──────────────────────── TLS setup ──────────────────────────

/**
 * @brief Reject TLS options this backend cannot honour, before any socket work.
 */
static az_result _check_tls_options(az_mqtt5_tls_options const* tls_options)
{
  if ((az_span_size(tls_options->client_cert_path) > 0)
      != (az_span_size(tls_options->client_key_path) > 0))
  {
    // Half a client identity would silently downgrade to server-only TLS.
    return AZ_MQTT5_ERROR_INVALID_CONFIG;
  }
  if (az_span_size(tls_options->ca_cert_path) == 0)
  {
    // mbedTLS has no system trust store; never connect without a trust anchor.
    return AZ_MQTT5_ERROR_NOT_SUPPORTED;
  }
  return AZ_OK;
}

/**
 * @brief Build the TLS configuration and session for @p host; the socket is attached later.
 */
static az_result _tls_prepare(
    az_mqtt5_transport* transport,
    az_span host,
    az_mqtt5_tls_options const* tls_options)
{
  static const char pers[] = "az_mqtt5_mbedtls";
  if (mbedtls_ctr_drbg_seed(
          &transport->ctr_drbg,
          mbedtls_entropy_func,
          &transport->entropy,
          (const unsigned char*)pers,
          sizeof(pers) - 1)
          != 0
      || mbedtls_ssl_config_defaults(
             &transport->conf,
             MBEDTLS_SSL_IS_CLIENT,
             MBEDTLS_SSL_TRANSPORT_STREAM,
             MBEDTLS_SSL_PRESET_DEFAULT)
          != 0)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }
  mbedtls_ssl_conf_rng(&transport->conf, mbedtls_ctr_drbg_random, &transport->ctr_drbg);

  char path[256];
  az_result rc = _span_to_cstr(tls_options->ca_cert_path, path, (int32_t)sizeof(path));
  if (az_result_failed(rc))
  {
    return rc;
  }
  if (mbedtls_x509_crt_parse_file(&transport->ca_chain, path) != 0)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }
  mbedtls_ssl_conf_ca_chain(&transport->conf, &transport->ca_chain, NULL);
  mbedtls_ssl_conf_authmode(&transport->conf, MBEDTLS_SSL_VERIFY_REQUIRED);

  if (az_span_size(tls_options->client_cert_path) > 0)
  {
    rc = _span_to_cstr(tls_options->client_cert_path, path, (int32_t)sizeof(path));
    if (az_result_failed(rc))
    {
      return rc;
    }
    if (mbedtls_x509_crt_parse_file(&transport->client_cert, path) != 0)
    {
      return AZ_MQTT5_ERROR_TRANSPORT;
    }
    rc = _span_to_cstr(tls_options->client_key_path, path, (int32_t)sizeof(path));
    if (az_result_failed(rc))
    {
      return rc;
    }
#if MBEDTLS_VERSION_MAJOR >= 3
    int const key_rc = mbedtls_pk_parse_keyfile(
        &transport->client_key, path, NULL, mbedtls_ctr_drbg_random, &transport->ctr_drbg);
#else
    int const key_rc = mbedtls_pk_parse_keyfile(&transport->client_key, path, NULL);
#endif
    if (key_rc != 0
        || mbedtls_ssl_conf_own_cert(
               &transport->conf, &transport->client_cert, &transport->client_key)
            != 0)
    {
      return AZ_MQTT5_ERROR_TRANSPORT;
    }
  }

  if (mbedtls_ssl_setup(&transport->ssl, &transport->conf) != 0)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }

  // Sets both SNI and the name the certificate is verified against.
  char host_str[256];
  rc = _span_to_cstr(host, host_str, (int32_t)sizeof(host_str));
  if (az_result_failed(rc))
  {
    return rc;
  }
  if (mbedtls_ssl_set_hostname(&transport->ssl, host_str) != 0)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }
  transport->use_tls = true;
  return AZ_OK;
}

/**
 * @brief Wait for what an mbedTLS call asked for: 1 retry, 0 timed out, -1 failed.
 */
static int _tls_wait(az_mqtt5_transport* transport, int ret, int64_t deadline)
{
  if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
  {
    return -1;
  }
  return _az_mqtt5_wait_fd(
      transport->socket_fd,
      ret == MBEDTLS_ERR_SSL_WANT_READ ? _AZ_MQTT5_WAIT_READ : _AZ_MQTT5_WAIT_WRITE,
      _az_mqtt5_remaining_ms(deadline));
}

/** @brief Drive the handshake until done, failed or @p deadline. */
static az_result _tls_handshake(az_mqtt5_transport* transport, int64_t deadline)
{
  for (;;)
  {
    int ret = mbedtls_ssl_handshake(&transport->ssl);
    if (ret == 0)
    {
      // VERIFY_REQUIRED already fails the handshake; checked again so it stays so.
      return mbedtls_ssl_get_verify_result(&transport->ssl) == 0 ? AZ_OK
                                                                 : AZ_MQTT5_ERROR_TRANSPORT;
    }
    int w = _tls_wait(transport, ret, deadline);
    if (w == 0)
    {
      return AZ_MQTT5_ERROR_TIMEOUT;
    }
    if (w < 0)
    {
      return AZ_MQTT5_ERROR_TRANSPORT;
    }
  }
}
#endif // AZ_MQTT5_TLS_MBEDTLS

// ──────────────────────── Connect ────────────────────────────

AZ_NODISCARD az_result az_mqtt5_transport_connect_start(
    az_mqtt5_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt5_tls_options const* tls_options)
{
  _az_PRECONDITION_NOT_NULL(transport);

  az_mqtt5_transport_close(transport);

#ifdef AZ_MQTT5_TLS_MBEDTLS
  if (tls_options != NULL)
  {
    az_result rc = _check_tls_options(tls_options);
    if (az_result_succeeded(rc))
    {
      rc = _tls_prepare(transport, host, tls_options);
    }
    if (az_result_failed(rc))
    {
      az_mqtt5_transport_close(transport);
      return rc;
    }
  }
#else
  if (tls_options != NULL)
  {
    // Built without TLS: never fall back to plaintext.
    return AZ_MQTT5_ERROR_NOT_SUPPORTED;
  }
#endif

  az_result rc = _az_mqtt5_tcp_connect_start(&transport->tcp, host, port);
  if (az_result_failed(rc))
  {
    az_mqtt5_transport_close(transport);
    return rc;
  }
  transport->state = _TRANSPORT_TCP;
  return AZ_OK;
}

AZ_NODISCARD az_result
az_mqtt5_transport_connect_poll(az_mqtt5_transport* transport, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(transport);
  int64_t const deadline = _az_mqtt5_deadline(timeout_ms);
  az_result rc = AZ_OK;

  if (transport->state == _TRANSPORT_TCP)
  {
    rc = _az_mqtt5_tcp_connect_poll(&transport->tcp, _az_mqtt5_remaining_ms(deadline));
    if (rc == AZ_MQTT5_ERROR_TIMEOUT)
    {
      return rc;
    }
    if (az_result_succeeded(rc))
    {
      transport->socket_fd = transport->tcp.fd;
      _az_mqtt5_tcp_connect_init(&transport->tcp);
      transport->state = _TRANSPORT_CONNECTED;
#ifdef AZ_MQTT5_TLS_MBEDTLS
      if (transport->use_tls)
      {
        mbedtls_ssl_set_bio(&transport->ssl, &transport->socket_fd, _tls_send, _tls_recv, NULL);
        transport->state = _TRANSPORT_TLS;
      }
#endif
    }
  }

#ifdef AZ_MQTT5_TLS_MBEDTLS
  if (az_result_succeeded(rc) && transport->state == _TRANSPORT_TLS)
  {
    rc = _tls_handshake(transport, deadline);
    if (rc == AZ_MQTT5_ERROR_TIMEOUT)
    {
      return rc;
    }
    if (az_result_succeeded(rc))
    {
      transport->state = _TRANSPORT_CONNECTED;
    }
  }
#endif

  if (az_result_succeeded(rc) && transport->state != _TRANSPORT_CONNECTED)
  {
    rc = AZ_MQTT5_ERROR_INVALID_STATE;
  }
  if (az_result_failed(rc))
  {
    az_mqtt5_transport_close(transport);
    return rc;
  }
  transport->connected = true;
  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_transport_connect(
    az_mqtt5_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt5_tls_options const* tls_options)
{
  az_result rc = az_mqtt5_transport_connect_start(transport, host, port, tls_options);
  if (az_result_succeeded(rc))
  {
    rc = az_mqtt5_transport_connect_poll(transport, AZ_MQTT5_TRANSPORT_CONNECT_TIMEOUT_MS);
    if (rc == AZ_MQTT5_ERROR_TIMEOUT)
    {
      az_mqtt5_transport_close(transport);
    }
  }
  return rc;
}

// ──────────────────────── I/O ────────────────────────────────

AZ_NODISCARD az_result az_mqtt5_transport_send(az_mqtt5_transport* transport, az_span data)
{
  _az_PRECONDITION_NOT_NULL(transport);
  if (!transport->connected)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }

  int64_t const deadline = _az_mqtt5_deadline(AZ_MQTT5_TRANSPORT_SEND_TIMEOUT_MS);
  uint8_t const* ptr = az_span_ptr(data);
  int32_t remaining = az_span_size(data);

  while (remaining > 0)
  {
    int w;
#ifdef AZ_MQTT5_TLS_MBEDTLS
    if (transport->use_tls)
    {
      int n = mbedtls_ssl_write(&transport->ssl, ptr, (size_t)remaining);
      if (n > 0)
      {
        ptr += n;
        remaining -= n;
        continue;
      }
      w = _tls_wait(transport, n, deadline);
    }
    else
#endif
    {
      int32_t n = _az_mqtt5_send_nosignal(transport->socket_fd, ptr, remaining);
      if (n > 0)
      {
        ptr += n;
        remaining -= n;
        continue;
      }
      w = n < 0 ? -1
                : _az_mqtt5_wait_fd(
                    transport->socket_fd, _AZ_MQTT5_WAIT_WRITE, _az_mqtt5_remaining_ms(deadline));
    }
    if (w <= 0)
    {
      // A partial packet may be on the wire: the connection is unusable.
      transport->connected = false;
      return w == 0 ? AZ_MQTT5_ERROR_TIMEOUT : AZ_MQTT5_ERROR_TRANSPORT;
    }
  }
  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_transport_receive(
    az_mqtt5_transport* transport,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received)
{
  _az_PRECONDITION_NOT_NULL(transport);
  _az_PRECONDITION_NOT_NULL(out_received);

  *out_received = AZ_SPAN_EMPTY;
  if (!transport->connected)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }

  int64_t const deadline = _az_mqtt5_deadline(timeout_ms);
  for (;;)
  {
    int w;
#ifdef AZ_MQTT5_TLS_MBEDTLS
    if (transport->use_tls)
    {
      // Read first: decrypted bytes may already be buffered inside mbedTLS.
      int n = mbedtls_ssl_read(&transport->ssl, az_span_ptr(buffer), (size_t)az_span_size(buffer));
      if (n > 0)
      {
        *out_received = az_span_slice(buffer, 0, n);
        return AZ_OK;
      }
      // A partial TLS record returns WANT_READ; wait for the rest within the deadline.
      w = _tls_wait(transport, n, deadline);
    }
    else
#endif
    {
      w = _az_mqtt5_wait_fd(
          transport->socket_fd, _AZ_MQTT5_WAIT_READ, _az_mqtt5_remaining_ms(deadline));
      if (w > 0)
      {
        int32_t n = _az_mqtt5_recv_nonblocking(
            transport->socket_fd, az_span_ptr(buffer), az_span_size(buffer));
        if (n > 0)
        {
          *out_received = az_span_slice(buffer, 0, n);
          return AZ_OK;
        }
        w = n < 0 ? -1 : 1;
      }
    }
    if (w == 0)
    {
      return AZ_OK; // Timed out: out_received stays empty.
    }
    if (w < 0)
    {
      transport->connected = false;
      return AZ_MQTT5_ERROR_TRANSPORT;
    }
  }
}

void az_mqtt5_transport_close(az_mqtt5_transport* transport)
{
  if (transport == NULL)
  {
    return;
  }
#ifdef AZ_MQTT5_TLS_MBEDTLS
  if (transport->use_tls && transport->connected)
  {
    (void)mbedtls_ssl_close_notify(&transport->ssl); // Best effort; non-blocking.
  }
  _tls_contexts_reset(transport);
#endif
  _az_mqtt5_tcp_connect_cancel(&transport->tcp);
  if (transport->socket_fd >= 0)
  {
    close(transport->socket_fd);
    transport->socket_fd = -1;
  }
  transport->state = _TRANSPORT_IDLE;
  transport->connected = false;
}
