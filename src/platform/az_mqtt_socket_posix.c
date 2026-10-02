// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_socket_posix.c
 * @brief Internal: non-blocking POSIX TCP helpers. See az_mqtt_socket_posix.h.
 */

#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE // clock_gettime, getaddrinfo, MSG_NOSIGNAL under -std=c99
#endif

#include "az_mqtt_socket_posix.h"

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__) || defined(ESP_PLATFORM)
#include <sys/random.h>
#define _AZ_MQTT_GETRANDOM
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <stdlib.h>
#define _AZ_MQTT_ARC4RANDOM
#endif

#ifndef MSG_NOSIGNAL
#ifdef SO_NOSIGPIPE
// No MSG_NOSIGNAL (e.g. macOS): SO_NOSIGPIPE is set on the socket instead.
#define MSG_NOSIGNAL 0
#else
#error "Neither MSG_NOSIGNAL nor SO_NOSIGPIPE: sends could raise SIGPIPE."
#endif
#endif

int64_t _az_mqtt_now_ms(void)
{
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

AZ_NODISCARD int64_t az_mqtt_transport_clock_ms(void) { return _az_mqtt_now_ms(); }

AZ_NODISCARD az_result az_mqtt_transport_random(az_span buffer)
{
  uint8_t* p = az_span_ptr(buffer);
  size_t left = (size_t)az_span_size(buffer);
#if defined(_AZ_MQTT_ARC4RANDOM)
  arc4random_buf(p, left);
#elif defined(_AZ_MQTT_GETRANDOM)
  while (left > 0)
  {
    ssize_t const n = getrandom(p, left, 0);
    if (n < 0 && errno != EINTR)
    {
      return AZ_MQTT_ERROR_TRANSPORT;
    }
    if (n > 0)
    {
      p += n;
      left -= (size_t)n;
    }
  }
#else
  int const fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }
  while (left > 0)
  {
    ssize_t const n = read(fd, p, left);
    if (n <= 0 && !(n < 0 && errno == EINTR))
    {
      (void)close(fd);
      return AZ_MQTT_ERROR_TRANSPORT;
    }
    if (n > 0)
    {
      p += n;
      left -= (size_t)n;
    }
  }
  (void)close(fd);
#endif
  return AZ_OK;
}

int64_t _az_mqtt_deadline(int32_t timeout_ms)
{
  return timeout_ms < 0 ? -1 : _az_mqtt_now_ms() + timeout_ms;
}

int32_t _az_mqtt_remaining_ms(int64_t deadline_ms)
{
  if (deadline_ms < 0)
  {
    return -1;
  }
  int64_t left = deadline_ms - _az_mqtt_now_ms();
  return left <= 0 ? 0 : (left > INT32_MAX ? INT32_MAX : (int32_t)left);
}

void _az_mqtt_report_error(
    _az_mqtt_error_sink const* sink,
    az_mqtt_native_error_source source,
    int32_t code,
    az_result result)
{
  if (sink->callback != NULL)
  {
    az_mqtt_native_error const error = { source, code, result, sink->connect_attempt };
    sink->callback(&error, sink->context);
  }
}

az_result _az_mqtt_socket_error(int err, _az_mqtt_error_sink const* sink)
{
  az_result const rc = _az_mqtt_errno_result(err);
  if (err != 0)
  {
    _az_mqtt_report_error(sink, AZ_MQTT_NATIVE_ERROR_SOCKET, err, rc);
  }
  return rc;
}

az_result _az_mqtt_errno_result(int err)
{
  switch (err)
  {
    case 0:
    case ECONNRESET:
    case ECONNABORTED:
    case EPIPE:
    case ENOTCONN:
      return AZ_MQTT_ERROR_CONNECTION_CLOSED;
    case ECONNREFUSED:
      return AZ_MQTT_ERROR_CONNECTION_REFUSED;
    default:
      return AZ_MQTT_ERROR_TRANSPORT;
  }
}

bool _az_mqtt_would_block(void)
{
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

int _az_mqtt_wait_fd(int fd, _az_mqtt_wait what, int32_t timeout_ms)
{
  int64_t const deadline = _az_mqtt_deadline(timeout_ms);
  for (;;)
  {
    struct pollfd p;
    p.fd = fd;
    p.events = (short)(((what & _AZ_MQTT_WAIT_READ) ? POLLIN : 0)
                       | ((what & _AZ_MQTT_WAIT_WRITE) ? POLLOUT : 0));
    p.revents = 0;
    int r = poll(&p, 1, _az_mqtt_remaining_ms(deadline));
    if (r >= 0)
    {
      // POLLERR/POLLHUP count as ready: the next I/O call reports the error.
      return r > 0 ? 1 : 0;
    }
    if (errno != EINTR)
    {
      return -1;
    }
  }
}

int32_t _az_mqtt_send_nosignal(int fd, uint8_t const* data, int32_t size)
{
  ssize_t n = send(fd, data, (size_t)size, MSG_NOSIGNAL);
  if (n >= 0)
  {
    return (int32_t)n;
  }
  return _az_mqtt_would_block() ? 0 : -1;
}

int32_t _az_mqtt_recv_nonblocking(int fd, uint8_t* buffer, int32_t size)
{
  ssize_t n = recv(fd, buffer, (size_t)size, 0);
  if (n > 0)
  {
    return (int32_t)n;
  }
  if (n < 0 && _az_mqtt_would_block())
  {
    return 0;
  }
  if (n == 0)
  {
    errno = 0; // Orderly close.
  }
  return -1;
}

void _az_mqtt_tcp_connect_init(_az_mqtt_tcp_connect* c)
{
  c->fd = -1;
  c->addresses = NULL;
  c->next = NULL;
  c->owns_addresses = false;
  c->attempt_start_ms = 0;
  c->last_errno = 0;
}

static void _release_addresses(_az_mqtt_tcp_connect* c)
{
  if (c->addresses != NULL && c->owns_addresses)
  {
    freeaddrinfo(c->addresses);
  }
  c->addresses = NULL;
  c->next = NULL;
  c->owns_addresses = false;
}

/** @brief Start a non-blocking connect to the next address that accepts one. */
/** @brief Record and report the failure of the current address. */
static void _fail_address(_az_mqtt_tcp_connect* c, int err, _az_mqtt_error_sink const* sink)
{
  c->last_errno = err;
  _az_mqtt_socket_error(err, sink);
}

static az_result _connect_next(_az_mqtt_tcp_connect* c, _az_mqtt_error_sink const* sink)
{
  while (c->next != NULL)
  {
    struct addrinfo* a = c->next;
    c->next = a->ai_next;

    int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0)
    {
      _fail_address(c, errno, sink);
      continue;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
    {
      _fail_address(c, errno, sink);
      close(fd);
      continue;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0)
    {
      _fail_address(c, errno, sink);
      close(fd); // It could raise SIGPIPE.
      continue;
    }
#endif
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0 || errno == EINPROGRESS)
    {
      c->fd = fd;
      c->attempt_start_ms = _az_mqtt_now_ms();
      return AZ_OK;
    }
    _fail_address(c, errno, sink);
    close(fd);
  }
  return AZ_MQTT_ERROR_TRANSPORT;
}

az_result _az_mqtt_tcp_connect_start(
    _az_mqtt_tcp_connect* c,
    az_span host,
    uint16_t port,
    _az_mqtt_error_sink const* sink)
{
  _az_mqtt_tcp_connect_cancel(c);

  char host_str[256];
  if (az_span_size(host) <= 0 || az_span_size(host) >= (int32_t)sizeof(host_str))
  {
    return AZ_ERROR_ARG;
  }
  memcpy(host_str, az_span_ptr(host), (size_t)az_span_size(host));
  host_str[az_span_size(host)] = '\0';

  char port_str[6];
  (void)snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* addresses = NULL;
  int const resolved = getaddrinfo(host_str, port_str, &hints, &addresses);
  if (resolved != 0 || addresses == NULL)
  {
    _az_mqtt_report_error(
        sink, AZ_MQTT_NATIVE_ERROR_NAME_RESOLUTION, resolved, AZ_MQTT_ERROR_NAME_RESOLUTION);
    return AZ_MQTT_ERROR_NAME_RESOLUTION;
  }
  az_result rc = _az_mqtt_tcp_connect_start_addresses(c, addresses, sink);
  if (az_result_succeeded(rc))
  {
    c->owns_addresses = true;
  }
  else
  {
    freeaddrinfo(addresses);
  }
  return rc;
}

az_result _az_mqtt_tcp_connect_start_addresses(
    _az_mqtt_tcp_connect* c,
    struct addrinfo* addresses,
    _az_mqtt_error_sink const* sink)
{
  _az_mqtt_tcp_connect_cancel(c);
  c->addresses = addresses;
  c->next = addresses;
  az_result rc = _connect_next(c, sink);
  if (az_result_failed(rc))
  {
    rc = _az_mqtt_errno_result(c->last_errno);
    _az_mqtt_tcp_connect_init(c);
  }
  return rc;
}

az_result _az_mqtt_tcp_connect_poll(
    _az_mqtt_tcp_connect* c,
    int32_t timeout_ms,
    _az_mqtt_error_sink const* sink)
{
  int64_t const deadline = _az_mqtt_deadline(timeout_ms);
  while (c->fd >= 0)
  {
    // With another address to try, give this one only its attempt budget.
    int32_t wait_ms = _az_mqtt_remaining_ms(deadline);
    bool const bounded_attempt = c->next != NULL;
    if (bounded_attempt)
    {
      int32_t const attempt_left
          = _az_mqtt_remaining_ms(c->attempt_start_ms + AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS);
      if (wait_ms < 0 || attempt_left < wait_ms)
      {
        wait_ms = attempt_left;
      }
    }
    int r = _az_mqtt_wait_fd(c->fd, _AZ_MQTT_WAIT_WRITE, wait_ms);
    if (r == 0
        && !(bounded_attempt
             && _az_mqtt_now_ms() - c->attempt_start_ms >= AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS))
    {
      return AZ_MQTT_ERROR_TIMEOUT;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (r > 0 && getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
    {
      int const fd = c->fd;
      _release_addresses(c);
      c->fd = fd; // Handed over: cancel() no longer closes it.
      return AZ_OK;
    }
    // Failed, or its attempt budget ran out: move on to the next address.
    _fail_address(c, r < 0 ? errno : (r == 0 ? ETIMEDOUT : (err != 0 ? err : errno)), sink);
    close(c->fd);
    c->fd = -1;
    if (az_result_failed(_connect_next(c, sink)))
    {
      break;
    }
  }
  az_result const rc = _az_mqtt_errno_result(c->last_errno);
  _az_mqtt_tcp_connect_cancel(c);
  return rc;
}

void _az_mqtt_tcp_connect_cancel(_az_mqtt_tcp_connect* c)
{
  if (c->fd >= 0 && c->addresses != NULL)
  {
    // Still connecting: the socket was never handed over.
    close(c->fd);
  }
  _release_addresses(c);
  _az_mqtt_tcp_connect_init(c);
}

// ──────────────────────── HTTP CONNECT tunnel ────────────────

#ifndef AZ_MQTT_NO_PROXY

az_result _az_mqtt_proxy_tunnel_start(
    _az_mqtt_proxy_tunnel* t,
    az_mqtt_proxy_options const* proxy,
    az_span host,
    uint16_t port)
{
  t->proxy = proxy;
  t->host = host;
  t->port = port;
  t->sent = 0;
  _az_mqtt_http_reply_init(&t->reply);
  uint8_t request[_AZ_MQTT_HTTP_CONNECT_REQUEST_MAX];
  az_result const rc = _az_mqtt_http_connect_request(
      proxy, host, port, AZ_SPAN_FROM_BUFFER(request), &t->request_size);
  az_span_fill(AZ_SPAN_FROM_BUFFER(request), 0);
  return rc;
}

/** @brief Report a failure of the tunnel's connection (errno @p err; 0: closed) as PROXY. */
static az_result _tunnel_socket_failure(int err, _az_mqtt_error_sink const* sink)
{
  if (err != 0)
  {
    _az_mqtt_report_error(sink, AZ_MQTT_NATIVE_ERROR_SOCKET, err, AZ_MQTT_ERROR_PROXY);
  }
  return AZ_MQTT_ERROR_PROXY;
}

/** @brief _az_mqtt_http_connect_send_fn over a socket; @p context is the int descriptor. */
static int32_t _tunnel_send(void* context, uint8_t const* data, int32_t size)
{
  return _az_mqtt_send_nosignal(*(int const*)context, data, size);
}

az_result _az_mqtt_proxy_tunnel_poll(
    _az_mqtt_proxy_tunnel* t,
    int fd,
    int32_t timeout_ms,
    _az_mqtt_error_sink const* sink)
{
  int64_t const deadline = _az_mqtt_deadline(timeout_ms);
  for (;;)
  {
    bool const sending = t->sent < t->request_size;
    if (sending)
    {
      az_result const sent = _az_mqtt_http_connect_send_request(
          t->proxy, t->host, t->port, &t->sent, _tunnel_send, &fd);
      if (sent == AZ_MQTT_ERROR_PROXY)
      {
        return _tunnel_socket_failure(errno, sink);
      }
      if (az_result_succeeded(sent))
      {
        continue;
      }
      if (sent != AZ_MQTT_ERROR_TIMEOUT)
      {
        return sent;
      }
    }
    int const w = _az_mqtt_wait_fd(
        fd, sending ? _AZ_MQTT_WAIT_WRITE : _AZ_MQTT_WAIT_READ, _az_mqtt_remaining_ms(deadline));
    if (w == 0)
    {
      return AZ_MQTT_ERROR_TIMEOUT;
    }
    if (w < 0)
    {
      return _tunnel_socket_failure(errno, sink);
    }
    if (sending)
    {
      continue;
    }
    // Peek, then take exactly the reply's bytes: what follows is the tunnelled stream's.
    uint8_t buffer[512];
    ssize_t const peeked = recv(fd, buffer, sizeof(buffer), MSG_PEEK);
    if (peeked <= 0)
    {
      if (peeked < 0 && _az_mqtt_would_block())
      {
        continue;
      }
      return _tunnel_socket_failure(peeked == 0 ? 0 : errno, sink);
    }
    int32_t consumed = 0;
    az_result const rc = _az_mqtt_http_connect_reply_parse(
        &t->reply, az_span_create(buffer, (int32_t)peeked), &consumed);
    if (recv(fd, buffer, (size_t)consumed, 0) != (ssize_t)consumed)
    {
      return _tunnel_socket_failure(errno, sink);
    }
    if (rc != AZ_MQTT_ERROR_TIMEOUT)
    {
      if (az_result_failed(rc))
      {
        _az_mqtt_report_error(sink, AZ_MQTT_NATIVE_ERROR_PROXY, t->reply.status, rc);
      }
      return rc;
    }
  }
}

#endif // AZ_MQTT_NO_PROXY
