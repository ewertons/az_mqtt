// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt5_socket_posix.c
 * @brief Internal: non-blocking POSIX TCP helpers. See az_mqtt5_socket_posix.h.
 */

#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE // clock_gettime, getaddrinfo, MSG_NOSIGNAL under -std=c99
#endif

#include "az_mqtt5_socket_posix.h"

#include <az_mqtt5/az_mqtt5_types.h>

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

#ifndef MSG_NOSIGNAL
#ifdef SO_NOSIGPIPE
// No MSG_NOSIGNAL (e.g. macOS): SO_NOSIGPIPE is set on the socket instead.
#define MSG_NOSIGNAL 0
#else
#error "Neither MSG_NOSIGNAL nor SO_NOSIGPIPE: sends could raise SIGPIPE."
#endif
#endif

int64_t _az_mqtt5_now_ms(void)
{
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

int64_t _az_mqtt5_deadline(int32_t timeout_ms)
{
  return timeout_ms < 0 ? -1 : _az_mqtt5_now_ms() + timeout_ms;
}

int32_t _az_mqtt5_remaining_ms(int64_t deadline_ms)
{
  if (deadline_ms < 0)
  {
    return -1;
  }
  int64_t left = deadline_ms - _az_mqtt5_now_ms();
  return left <= 0 ? 0 : (left > INT32_MAX ? INT32_MAX : (int32_t)left);
}

bool _az_mqtt5_would_block(void)
{
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

int _az_mqtt5_wait_fd(int fd, _az_mqtt5_wait what, int32_t timeout_ms)
{
  int64_t const deadline = _az_mqtt5_deadline(timeout_ms);
  for (;;)
  {
    struct pollfd p;
    p.fd = fd;
    p.events = (short)(((what & _AZ_MQTT5_WAIT_READ) ? POLLIN : 0)
                       | ((what & _AZ_MQTT5_WAIT_WRITE) ? POLLOUT : 0));
    p.revents = 0;
    int r = poll(&p, 1, _az_mqtt5_remaining_ms(deadline));
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

int32_t _az_mqtt5_send_nosignal(int fd, uint8_t const* data, int32_t size)
{
  ssize_t n = send(fd, data, (size_t)size, MSG_NOSIGNAL);
  if (n >= 0)
  {
    return (int32_t)n;
  }
  return _az_mqtt5_would_block() ? 0 : -1;
}

int32_t _az_mqtt5_recv_nonblocking(int fd, uint8_t* buffer, int32_t size)
{
  ssize_t n = recv(fd, buffer, (size_t)size, 0);
  if (n > 0)
  {
    return (int32_t)n;
  }
  if (n < 0 && _az_mqtt5_would_block())
  {
    return 0;
  }
  return -1;
}

void _az_mqtt5_tcp_connect_init(_az_mqtt5_tcp_connect* c)
{
  c->fd = -1;
  c->addresses = NULL;
  c->next = NULL;
}

/** @brief Start a non-blocking connect to the next address that accepts one. */
static az_result _connect_next(_az_mqtt5_tcp_connect* c)
{
  while (c->next != NULL)
  {
    struct addrinfo* a = c->next;
    c->next = a->ai_next;

    int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0)
    {
      continue;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
    {
      close(fd);
      continue;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0 || errno == EINPROGRESS)
    {
      c->fd = fd;
      return AZ_OK;
    }
    close(fd);
  }
  return AZ_MQTT5_ERROR_TRANSPORT;
}

az_result _az_mqtt5_tcp_connect_start(_az_mqtt5_tcp_connect* c, az_span host, uint16_t port)
{
  _az_mqtt5_tcp_connect_cancel(c);

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
  if (getaddrinfo(host_str, port_str, &hints, &c->addresses) != 0 || c->addresses == NULL)
  {
    c->addresses = NULL;
    return AZ_MQTT5_ERROR_TRANSPORT;
  }
  c->next = c->addresses;

  az_result rc = _connect_next(c);
  if (az_result_failed(rc))
  {
    _az_mqtt5_tcp_connect_cancel(c);
  }
  return rc;
}

az_result _az_mqtt5_tcp_connect_poll(_az_mqtt5_tcp_connect* c, int32_t timeout_ms)
{
  int64_t const deadline = _az_mqtt5_deadline(timeout_ms);
  while (c->fd >= 0)
  {
    int r = _az_mqtt5_wait_fd(c->fd, _AZ_MQTT5_WAIT_WRITE, _az_mqtt5_remaining_ms(deadline));
    if (r == 0)
    {
      return AZ_MQTT5_ERROR_TIMEOUT;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (r > 0 && getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
    {
      if (c->addresses != NULL)
      {
        freeaddrinfo(c->addresses);
      }
      c->addresses = NULL;
      c->next = NULL;
      return AZ_OK;
    }
    close(c->fd);
    c->fd = -1;
    if (az_result_failed(_connect_next(c)))
    {
      break;
    }
  }
  _az_mqtt5_tcp_connect_cancel(c);
  return AZ_MQTT5_ERROR_TRANSPORT;
}

void _az_mqtt5_tcp_connect_cancel(_az_mqtt5_tcp_connect* c)
{
  if (c->fd >= 0 && c->addresses != NULL)
  {
    // Still connecting: the socket was never handed over.
    close(c->fd);
  }
  if (c->addresses != NULL)
  {
    freeaddrinfo(c->addresses);
  }
  _az_mqtt5_tcp_connect_init(c);
}
