// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "e2e_proxy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET _socket_t;
#define _BAD_SOCKET INVALID_SOCKET
#define _close_socket closesocket
typedef HANDLE _thread_t;
typedef CRITICAL_SECTION _mutex_t;
static void _sleep_ms(int ms) { Sleep((DWORD)ms); }
static void _mutex_init(_mutex_t* m) { InitializeCriticalSection(m); }
static void _mutex_destroy(_mutex_t* m) { DeleteCriticalSection(m); }
static void _lock(_mutex_t* m) { EnterCriticalSection(m); }
static void _unlock(_mutex_t* m) { LeaveCriticalSection(m); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int _socket_t;
#define _BAD_SOCKET (-1)
#define _close_socket close
typedef pthread_t _thread_t;
typedef pthread_mutex_t _mutex_t;
static void _sleep_ms(int ms)
{
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}
static void _mutex_init(_mutex_t* m) { pthread_mutex_init(m, NULL); }
static void _mutex_destroy(_mutex_t* m) { pthread_mutex_destroy(m); }
static void _lock(_mutex_t* m) { pthread_mutex_lock(m); }
static void _unlock(_mutex_t* m) { pthread_mutex_unlock(m); }
#endif

struct e2e_proxy
{
  e2e_proxy_options options;
  _socket_t listener;
  uint16_t port;
  _thread_t thread;
  _mutex_t lock;
  bool stop; ///< Under lock.
  int tunnels; ///< Under lock.
  int client_alerts; ///< Under lock.
  bool reset_requested; ///< Under lock.
  bool reset_done; ///< Under lock.
};

/** @brief TLS record framing of the client's stream: header bytes seen, body bytes left. */
typedef struct
{
  uint8_t header[5];
  int header_len;
  int body_left;
} _records;

/** @brief Follow the record framing over @p data; count alert records. */
static void _scan_records(e2e_proxy* p, _records* r, uint8_t const* data, int size)
{
  while (size > 0)
  {
    if (r->body_left > 0)
    {
      int const n = size < r->body_left ? size : r->body_left;
      r->body_left -= n;
      data += n;
      size -= n;
      continue;
    }
    r->header[r->header_len++] = *data++;
    size--;
    if (r->header_len == 5)
    {
      r->header_len = 0;
      r->body_left = (r->header[3] << 8) | r->header[4];
      if (r->header[0] == 21)
      {
        _lock(&p->lock);
        p->client_alerts++;
        _unlock(&p->lock);
      }
    }
  }
}

/** @brief Close @p fd with a reset (no FIN). */
static void _reset(_socket_t fd)
{
  struct linger l;
  l.l_onoff = 1;
  l.l_linger = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, (char const*)&l, (int)sizeof(l));
  _close_socket(fd);
}

static bool _stopping(e2e_proxy* p)
{
  _lock(&p->lock);
  bool const stop = p->stop;
  _unlock(&p->lock);
  return stop;
}

/** @brief Wait up to 50 ms for @p fd to be readable: 1 ready, 0 not yet, -1 failed. */
static int _readable(_socket_t fd)
{
  fd_set set;
  FD_ZERO(&set);
  FD_SET(fd, &set);
  struct timeval tv = { 0, 50000 };
  return select((int)fd + 1, &set, NULL, NULL, &tv);
}

#ifdef MSG_NOSIGNAL
#define _SEND_FLAGS MSG_NOSIGNAL // A closed peer must not raise SIGPIPE.
#else
#define _SEND_FLAGS 0
#endif

static bool _send_all(_socket_t fd, char const* data, int size)
{
  while (size > 0)
  {
    int n = (int)send(fd, data, size, _SEND_FLAGS);
    if (n <= 0)
    {
      return false;
    }
    data += n;
    size -= n;
  }
  return true;
}

/** @brief Read the request headers one byte at a time (nothing past them); false on close. */
static bool _read_request(e2e_proxy* p, _socket_t fd, char* head, int size)
{
  int len = 0;
  while (len + 1 < size && !_stopping(p))
  {
    int const r = _readable(fd);
    if (r < 0)
    {
      return false;
    }
    if (r == 0)
    {
      continue;
    }
    if (recv(fd, head + len, 1, 0) != 1)
    {
      return false;
    }
    head[++len] = '\0';
    if (len >= 4 && memcmp(head + len - 4, "\r\n\r\n", 4) == 0)
    {
      return true;
    }
  }
  return false;
}

static void _reply(e2e_proxy* p, _socket_t fd, char const* reply)
{
  _sleep_ms(p->options.reply_delay_ms);
  int const len = (int)strlen(reply);
  if (!p->options.fragment_reply)
  {
    (void)_send_all(fd, reply, len);
    return;
  }
  for (int i = 0; i < len && !_stopping(p); i++)
  {
    (void)_send_all(fd, reply + i, 1);
    _sleep_ms(p->options.byte_delay_ms);
  }
}

static _socket_t _connect_target(char const* authority)
{
  char host[256];
  char const* colon = strrchr(authority, ':');
  if (colon == NULL || (size_t)(colon - authority) >= sizeof(host))
  {
    return _BAD_SOCKET;
  }
  memcpy(host, authority, (size_t)(colon - authority));
  host[colon - authority] = '\0';
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = NULL;
  if (getaddrinfo(host, colon + 1, &hints, &res) != 0 || res == NULL)
  {
    return _BAD_SOCKET;
  }
  _socket_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd != _BAD_SOCKET && connect(fd, res->ai_addr, (int)res->ai_addrlen) != 0)
  {
    _close_socket(fd);
    fd = _BAD_SOCKET;
  }
  freeaddrinfo(res);
  return fd;
}

/** @brief Relay client @p a and target @p b; false if @p a was reset (and closed). */
static bool _pump(e2e_proxy* p, _socket_t a, _socket_t b)
{
  char buf[4096];
  _records records = { { 0 }, 0, 0 };
  while (!_stopping(p))
  {
    _lock(&p->lock);
    bool const reset = p->reset_requested;
    _unlock(&p->lock);
    if (reset)
    {
      _reset(a);
      _lock(&p->lock);
      p->reset_done = true;
      _unlock(&p->lock);
      return false;
    }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(a, &set);
    FD_SET(b, &set);
    struct timeval tv = { 0, 50000 };
    int const r = select((int)(a > b ? a : b) + 1, &set, NULL, NULL, &tv);
    if (r < 0)
    {
      return true;
    }
    _socket_t const ends[2][2] = { { a, b }, { b, a } };
    for (int i = 0; i < 2; i++)
    {
      if (FD_ISSET(ends[i][0], &set))
      {
        int const n = (int)recv(ends[i][0], buf, (int)sizeof(buf), 0);
        if (n <= 0 || !_send_all(ends[i][1], buf, n))
        {
          return true;
        }
        if (i == 0)
        {
          _scan_records(p, &records, (uint8_t const*)buf, n);
        }
      }
    }
  }
  return true;
}

/** @brief Serve one client; false if its connection was reset (and closed). */
static bool _serve(e2e_proxy* p, _socket_t fd)
{
  char head[4096];
  char authority[300];
  if (!_read_request(p, fd, head, (int)sizeof(head)) || strncmp(head, "CONNECT ", 8) != 0)
  {
    return true;
  }
  char const* const end = strchr(head + 8, ' ');
  size_t const len = end == NULL ? 0 : (size_t)(end - (head + 8));
  if (len == 0 || len >= sizeof(authority))
  {
    return true;
  }
  memcpy(authority, head + 8, len);
  authority[len] = '\0';
  if (p->options.refuse_auth)
  {
    _reply(p, fd, "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\n\r\n");
    return true;
  }
  _socket_t const target = _connect_target(authority);
  if (target == _BAD_SOCKET)
  {
    _reply(p, fd, "HTTP/1.1 502 Bad Gateway\r\n\r\n");
    return true;
  }
  _lock(&p->lock);
  p->tunnels++;
  p->client_alerts = 0;
  p->reset_requested = false;
  p->reset_done = false;
  _unlock(&p->lock);
  _reply(p, fd, "HTTP/1.1 200 Connection established\r\nVia: 1.1 e2e-proxy\r\n\r\n");
  bool const open = _pump(p, fd, target);
  _close_socket(target);
  return open;
}

static void _run(e2e_proxy* p)
{
  while (!_stopping(p))
  {
    if (_readable(p->listener) <= 0)
    {
      continue;
    }
    _socket_t const fd = accept(p->listener, NULL, NULL);
    if (fd != _BAD_SOCKET && _serve(p, fd))
    {
      _close_socket(fd);
    }
  }
}

#ifdef _WIN32
static DWORD WINAPI _thread_main(LPVOID arg)
{
  _run((e2e_proxy*)arg);
  return 0;
}
#else
static void* _thread_main(void* arg)
{
  _run((e2e_proxy*)arg);
  return NULL;
}
#endif

e2e_proxy* e2e_proxy_start(e2e_proxy_options const* options)
{
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
  {
    return NULL;
  }
#endif
  e2e_proxy* p = (e2e_proxy*)calloc(1, sizeof(*p));
  if (p == NULL)
  {
    return NULL;
  }
  p->options = *options;
  _mutex_init(&p->lock);
  p->listener = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = (socklen_t)sizeof(sa);
  bool ok = p->listener != _BAD_SOCKET
      && bind(p->listener, (struct sockaddr*)&sa, (int)sizeof(sa)) == 0
      && listen(p->listener, 4) == 0
      && getsockname(p->listener, (struct sockaddr*)&sa, &len) == 0;
#ifdef _WIN32
  ok = ok && (p->thread = CreateThread(NULL, 0, _thread_main, p, 0, NULL)) != NULL;
#else
  ok = ok && pthread_create(&p->thread, NULL, _thread_main, p) == 0;
#endif
  if (!ok)
  {
    if (p->listener != _BAD_SOCKET)
    {
      _close_socket(p->listener);
    }
    _mutex_destroy(&p->lock);
    free(p);
    return NULL;
  }
  p->port = ntohs(sa.sin_port);
  return p;
}

uint16_t e2e_proxy_port(e2e_proxy const* p) { return p->port; }

int e2e_proxy_tunnels(e2e_proxy* p)
{
  _lock(&p->lock);
  int const n = p->tunnels;
  _unlock(&p->lock);
  return n;
}

int e2e_proxy_client_alerts(e2e_proxy* p)
{
  _lock(&p->lock);
  int const n = p->client_alerts;
  _unlock(&p->lock);
  return n;
}

bool e2e_proxy_reset_client(e2e_proxy* p)
{
  for (int i = 0; i < 100; i++)
  {
    _lock(&p->lock);
    p->reset_requested = true;
    bool const done = p->reset_done;
    _unlock(&p->lock);
    if (done)
    {
      return true;
    }
    _sleep_ms(20);
  }
  return false;
}

void e2e_proxy_stop(e2e_proxy* p)
{
  if (p == NULL)
  {
    return;
  }
  _lock(&p->lock);
  p->stop = true;
  _unlock(&p->lock);
#ifdef _WIN32
  WaitForSingleObject(p->thread, INFINITE);
  CloseHandle(p->thread);
#else
  pthread_join(p->thread, NULL);
#endif
  _close_socket(p->listener);
  _mutex_destroy(&p->lock);
  free(p);
#ifdef _WIN32
  WSACleanup();
#endif
}
