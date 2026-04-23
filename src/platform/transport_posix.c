// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

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

#ifdef AZ_MQTT5_TLS_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

// ──────────────────────── Platform-specific transport ────────

struct az_mqtt5_transport
{
  int socket_fd;
#ifdef AZ_MQTT5_TLS_OPENSSL
  SSL_CTX* ssl_ctx;
  SSL* ssl;
#endif
  bool connected;
};

AZ_NODISCARD int32_t az_mqtt5_transport_sizeof(void) { return (int32_t)sizeof(az_mqtt5_transport); }

AZ_NODISCARD az_result az_mqtt5_transport_init(az_mqtt5_transport* transport)
{
  _az_PRECONDITION_NOT_NULL(transport);
  transport->socket_fd = -1;
#ifdef AZ_MQTT5_TLS_OPENSSL
  transport->ssl_ctx = NULL;
  transport->ssl = NULL;
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
  return AZ_OK;
}

#ifdef AZ_MQTT5_TLS_OPENSSL
static az_result _tls_setup(
    az_mqtt5_transport* transport,
    az_span host,
    az_mqtt5_tls_options const* tls_options)
{
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (ctx == NULL)
  {
    return AZ_MQTT5_ERROR_TRANSPORT;
  }

  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

  if (az_span_size(tls_options->ca_cert_path) > 0)
  {
    char ca_path[256];
    az_result rc = _span_to_cstr(tls_options->ca_cert_path, ca_path, (int32_t)sizeof(ca_path));
    if (az_result_failed(rc))
    {
      SSL_CTX_free(ctx);
      return rc;
    }
    if (SSL_CTX_load_verify_locations(ctx, ca_path, NULL) != 1)
    {
      SSL_CTX_free(ctx);
      return AZ_MQTT5_ERROR_TRANSPORT;
    }
  }
  else
  {
    SSL_CTX_set_default_verify_paths(ctx);
  }

  if (az_span_size(tls_options->client_cert_path) > 0)
  {
    char cert_path[256];
    az_result rc
        = _span_to_cstr(tls_options->client_cert_path, cert_path, (int32_t)sizeof(cert_path));
    if (az_result_failed(rc))
    {
      SSL_CTX_free(ctx);
      return rc;
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1)
    {
      SSL_CTX_free(ctx);
      return AZ_MQTT5_ERROR_TRANSPORT;
    }
  }

  if (az_span_size(tls_options->client_key_path) > 0)
  {
    char key_path[256];
    az_result rc
        = _span_to_cstr(tls_options->client_key_path, key_path, (int32_t)sizeof(key_path));
    if (az_result_failed(rc))
    {
      SSL_CTX_free(ctx);
      return rc;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1)
    {
      SSL_CTX_free(ctx);
      return AZ_MQTT5_ERROR_TRANSPORT;
    }
  }

  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

  SSL* ssl = SSL_new(ctx);
  if (ssl == NULL)
  {
    SSL_CTX_free(ctx);
    return AZ_MQTT5_ERROR_TRANSPORT;
  }

  SSL_set_fd(ssl, transport->socket_fd);

  // Set SNI hostname
  char host_str[256];
  az_result rc = _span_to_cstr(host, host_str, (int32_t)sizeof(host_str));
  if (az_result_failed(rc))
  {
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return rc;
  }
  SSL_set_tlsext_host_name(ssl, host_str);

  if (SSL_connect(ssl) != 1)
  {
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return AZ_MQTT5_ERROR_TRANSPORT;
  }

  transport->ssl_ctx = ctx;
  transport->ssl = ssl;
  return AZ_OK;
}
#endif // AZ_MQTT5_TLS_OPENSSL

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

#ifdef AZ_MQTT5_TLS_OPENSSL
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
#ifdef AZ_MQTT5_TLS_OPENSSL
    if (transport->ssl != NULL)
    {
      sent = SSL_write(transport->ssl, ptr, remaining);
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

#ifdef AZ_MQTT5_TLS_OPENSSL
  // Check for pending SSL data first
  if (transport->ssl != NULL && SSL_pending(transport->ssl) > 0)
  {
    int n = SSL_read(transport->ssl, az_span_ptr(buffer), az_span_size(buffer));
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
#ifdef AZ_MQTT5_TLS_OPENSSL
  if (transport->ssl != NULL)
  {
    n = SSL_read(transport->ssl, az_span_ptr(buffer), az_span_size(buffer));
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
#ifdef AZ_MQTT5_TLS_OPENSSL
  if (transport->ssl != NULL)
  {
    SSL_shutdown(transport->ssl);
    SSL_free(transport->ssl);
    transport->ssl = NULL;
  }
  if (transport->ssl_ctx != NULL)
  {
    SSL_CTX_free(transport->ssl_ctx);
    transport->ssl_ctx = NULL;
  }
#endif
  if (transport->socket_fd >= 0)
  {
    close(transport->socket_fd);
    transport->socket_fd = -1;
  }
  transport->connected = false;
}
