// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

// POSIX TCP transport with optional mbedTLS TLS layer (alternative to
// transport_posix.c which uses OpenSSL). Selected via the
// `AZ_MQTT5_TLS_BACKEND=mbedtls` CMake option; the compile-time macro
// `AZ_MQTT5_TLS_MBEDTLS` gates the TLS code-paths.

#include <az_mqtt5/az_mqtt5_transport.h>
#include <az_mqtt5/az_mqtt5_types.h>

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/az_span.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef AZ_MQTT5_TLS_MBEDTLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#endif

// ──────────────────────── Platform-specific transport ────────

struct az_mqtt5_transport
{
  int socket_fd;
#ifdef AZ_MQTT5_TLS_MBEDTLS
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config  conf;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_x509_crt ca_chain;
  mbedtls_x509_crt client_cert;
  mbedtls_pk_context client_key;
  mbedtls_net_context net_ctx;
  bool tls_initialized;
#endif
  bool connected;
};

AZ_NODISCARD int32_t az_mqtt5_transport_sizeof(void) { return (int32_t)sizeof(az_mqtt5_transport); }

AZ_NODISCARD az_result az_mqtt5_transport_init(az_mqtt5_transport* transport)
{
  _az_PRECONDITION_NOT_NULL(transport);
  transport->socket_fd = -1;
#ifdef AZ_MQTT5_TLS_MBEDTLS
  mbedtls_ssl_init(&transport->ssl);
  mbedtls_ssl_config_init(&transport->conf);
  mbedtls_entropy_init(&transport->entropy);
  mbedtls_ctr_drbg_init(&transport->ctr_drbg);
  mbedtls_x509_crt_init(&transport->ca_chain);
  mbedtls_x509_crt_init(&transport->client_cert);
  mbedtls_pk_init(&transport->client_key);
  mbedtls_net_init(&transport->net_ctx);
  transport->tls_initialized = false;
#endif
  transport->connected = false;
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

static az_result _tcp_connect(az_mqtt5_transport* transport, az_span host, uint16_t port)
{
  char host_str[256];
  az_result rc = _span_to_cstr(host, host_str, (int32_t)sizeof(host_str));
  if (az_result_failed(rc))
  {
    return rc;
  }

  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res = NULL;
  if (getaddrinfo(host_str, port_str, &hints, &res) != 0 || res == NULL)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }

  int fd = -1;
  for (struct addrinfo* rp = res; rp != NULL; rp = rp->ai_next)
  {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0)
    {
      continue;
    }
    if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
    {
      break;
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);

  if (fd < 0)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }

  transport->socket_fd = fd;
#ifdef AZ_MQTT5_TLS_MBEDTLS
  transport->net_ctx.fd = fd;
#endif
  return AZ_OK;
}

#ifdef AZ_MQTT5_TLS_MBEDTLS
static az_result _tls_setup(
    az_mqtt5_transport* transport,
    az_span host,
    az_mqtt5_tls_options const* tls_options)
{
  static const char* pers = "az_mqtt5_mbedtls";
  int ret = mbedtls_ctr_drbg_seed(
      &transport->ctr_drbg,
      mbedtls_entropy_func,
      &transport->entropy,
      (const unsigned char*)pers,
      strlen(pers));
  if (ret != 0)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }

  ret = mbedtls_ssl_config_defaults(
      &transport->conf,
      MBEDTLS_SSL_IS_CLIENT,
      MBEDTLS_SSL_TRANSPORT_STREAM,
      MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }

  mbedtls_ssl_conf_rng(&transport->conf, mbedtls_ctr_drbg_random, &transport->ctr_drbg);

  if (az_span_size(tls_options->ca_cert_path) > 0)
  {
    char ca_path[256];
    az_result rc = _span_to_cstr(tls_options->ca_cert_path, ca_path, (int32_t)sizeof(ca_path));
    if (az_result_failed(rc))
    {
      return rc;
    }
    if (mbedtls_x509_crt_parse_file(&transport->ca_chain, ca_path) != 0)
    {
      return AZ_MQTT5_ERROR_TRANSPORT;
    }
    mbedtls_ssl_conf_ca_chain(&transport->conf, &transport->ca_chain, NULL);
    mbedtls_ssl_conf_authmode(&transport->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  }
  else
  {
    // No CA provided — skip verification. Production code should always verify.
    mbedtls_ssl_conf_authmode(&transport->conf, MBEDTLS_SSL_VERIFY_NONE);
  }

  if (az_span_size(tls_options->client_cert_path) > 0
      && az_span_size(tls_options->client_key_path) > 0)
  {
    char cert_path[256];
    char key_path[256];
    az_result rc
        = _span_to_cstr(tls_options->client_cert_path, cert_path, (int32_t)sizeof(cert_path));
    if (az_result_failed(rc))
    {
      return rc;
    }
    rc = _span_to_cstr(tls_options->client_key_path, key_path, (int32_t)sizeof(key_path));
    if (az_result_failed(rc))
    {
      return rc;
    }
    if (mbedtls_x509_crt_parse_file(&transport->client_cert, cert_path) != 0)
    {
      return AZ_MQTT5_ERROR_TRANSPORT;
    }
#if MBEDTLS_VERSION_MAJOR >= 3
    if (mbedtls_pk_parse_keyfile(
            &transport->client_key, key_path, NULL,
            mbedtls_ctr_drbg_random, &transport->ctr_drbg)
        != 0)
#else
    if (mbedtls_pk_parse_keyfile(&transport->client_key, key_path, NULL) != 0)
#endif
    {
      return AZ_MQTT5_ERROR_TRANSPORT;
    }
    if (mbedtls_ssl_conf_own_cert(
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

  // Set SNI hostname
  char host_str[256];
  az_result rc = _span_to_cstr(host, host_str, (int32_t)sizeof(host_str));
  if (az_result_failed(rc))
  {
    return rc;
  }
  if (mbedtls_ssl_set_hostname(&transport->ssl, host_str) != 0)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }

  mbedtls_ssl_set_bio(
      &transport->ssl,
      &transport->net_ctx,
      mbedtls_net_send,
      mbedtls_net_recv,
      NULL);

  int hs;
  while ((hs = mbedtls_ssl_handshake(&transport->ssl)) != 0)
  {
    if (hs != MBEDTLS_ERR_SSL_WANT_READ && hs != MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      return AZ_MQTT5_ERROR_TRANSPORT;
    }
  }

  transport->tls_initialized = true;
  return AZ_OK;
}
#endif // AZ_MQTT5_TLS_MBEDTLS

AZ_NODISCARD az_result az_mqtt5_transport_connect(
    az_mqtt5_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt5_tls_options const* tls_options)
{
  _az_PRECONDITION_NOT_NULL(transport);

  az_result rc = _tcp_connect(transport, host, port);
  if (az_result_failed(rc))
  {
    return rc;
  }

#ifdef AZ_MQTT5_TLS_MBEDTLS
  if (tls_options != NULL)
  {
    rc = _tls_setup(transport, host, tls_options);
    if (az_result_failed(rc))
    {
      close(transport->socket_fd);
      transport->socket_fd = -1;
      return rc;
    }
  }
#else
  (void)tls_options;
#endif

  transport->connected = true;
  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_transport_send(az_mqtt5_transport* transport, az_span data)
{
  _az_PRECONDITION_NOT_NULL(transport);

  uint8_t* ptr = az_span_ptr(data);
  int32_t remaining = az_span_size(data);

  while (remaining > 0)
  {
    ssize_t sent;
#ifdef AZ_MQTT5_TLS_MBEDTLS
    if (transport->tls_initialized)
    {
      int w = mbedtls_ssl_write(&transport->ssl, ptr, (size_t)remaining);
      if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE)
      {
        continue;
      }
      sent = (ssize_t)w;
    }
    else
#endif
    {
      sent = send(transport->socket_fd, ptr, (size_t)remaining, 0);
    }

    if (sent <= 0)
    {
      return AZ_MQTT5_ERROR_TRANSPORT;
    }
    ptr += sent;
    remaining -= (int32_t)sent;
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

#ifdef AZ_MQTT5_TLS_MBEDTLS
  // Check for pending TLS data first
  if (transport->tls_initialized
      && mbedtls_ssl_get_bytes_avail(&transport->ssl) > 0)
  {
    int n = mbedtls_ssl_read(
        &transport->ssl, az_span_ptr(buffer), (size_t)az_span_size(buffer));
    if (n > 0)
    {
      *out_received = az_span_slice(buffer, 0, n);
      return AZ_OK;
    }
  }
#endif

  struct pollfd pfd;
  pfd.fd = transport->socket_fd;
  pfd.events = POLLIN;
  pfd.revents = 0;

  int poll_result = poll(&pfd, 1, timeout_ms);
  if (poll_result < 0)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }
  if (poll_result == 0)
  {
    // Timeout: out_received is already empty
    return AZ_OK;
  }

  ssize_t n;
#ifdef AZ_MQTT5_TLS_MBEDTLS
  if (transport->tls_initialized)
  {
    int r = mbedtls_ssl_read(
        &transport->ssl, az_span_ptr(buffer), (size_t)az_span_size(buffer));
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      return AZ_OK;
    }
    n = (ssize_t)r;
  }
  else
#endif
  {
    n = recv(transport->socket_fd, az_span_ptr(buffer), (size_t)az_span_size(buffer), 0);
  }

  if (n < 0)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }
  if (n == 0)
  {
    // Connection closed
    transport->connected = false;
    return AZ_MQTT5_ERROR_TRANSPORT;
  }

  *out_received = az_span_slice(buffer, 0, (int32_t)n);
  return AZ_OK;
}

void az_mqtt5_transport_close(az_mqtt5_transport* transport)
{
  if (transport == NULL)
  {
    return;
  }
#ifdef AZ_MQTT5_TLS_MBEDTLS
  if (transport->tls_initialized)
  {
    (void)mbedtls_ssl_close_notify(&transport->ssl);
    transport->tls_initialized = false;
  }
  mbedtls_ssl_free(&transport->ssl);
  mbedtls_ssl_config_free(&transport->conf);
  mbedtls_x509_crt_free(&transport->ca_chain);
  mbedtls_x509_crt_free(&transport->client_cert);
  mbedtls_pk_free(&transport->client_key);
  mbedtls_ctr_drbg_free(&transport->ctr_drbg);
  mbedtls_entropy_free(&transport->entropy);
  mbedtls_net_free(&transport->net_ctx);
#endif
  if (transport->socket_fd >= 0)
  {
    close(transport->socket_fd);
    transport->socket_fd = -1;
  }
  transport->connected = false;
}
