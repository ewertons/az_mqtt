// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "test_proxy.h"

#include <openssl/evp.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct test_proxy
{
  test_proxy_options options;
  int listener;
  uint16_t port;
  pthread_t thread;
  pthread_mutex_t lock;
  bool stop; ///< Under lock: read with _stopping().
  int requests;
  int tunnels;
  int auth_failures;
  char last_request_line[512];
};

static bool _stopping(test_proxy* p)
{
  pthread_mutex_lock(&p->lock);
  bool const stop = p->stop;
  pthread_mutex_unlock(&p->lock);
  return stop;
}

static void _sleep_ms(int ms)
{
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}

static bool _send_all(int fd, char const* data, size_t size)
{
  while (size > 0)
  {
    ssize_t n = send(fd, data, size, MSG_NOSIGNAL);
    if (n <= 0)
    {
      return false;
    }
    data += n;
    size -= (size_t)n;
  }
  return true;
}

/** @brief Read the request headers (up to the empty line), byte by byte; false on close. */
static bool _read_request(test_proxy* p, int fd, char* head, size_t size)
{
  size_t len = 0;
  while (len + 1 < size)
  {
    struct pollfd pfd = { fd, POLLIN, 0 };
    int r = poll(&pfd, 1, 50);
    if (_stopping(p))
    {
      return false;
    }
    if (r <= 0)
    {
      continue;
    }
    if (recv(fd, head + len, 1, 0) != 1)
    {
      return false;
    }
    len++;
    head[len] = '\0';
    if (len >= 4 && memcmp(head + len - 4, "\r\n\r\n", 4) == 0)
    {
      return true;
    }
  }
  return false;
}

/** @brief The value of header @p name in @p head (case-insensitive name), copied to @p out. */
static bool _header(char const* head, char const* name, char* out, size_t size)
{
  size_t const name_len = strlen(name);
  for (char const* line = strstr(head, "\r\n"); line != NULL && line[2] != '\r';
       line = strstr(line + 2, "\r\n"))
  {
    char const* start = line + 2;
    if (strncasecmp(start, name, name_len) == 0 && start[name_len] == ':')
    {
      char const* value = start + name_len + 1;
      while (*value == ' ')
      {
        value++;
      }
      char const* end = strstr(value, "\r\n");
      size_t n = (size_t)(end - value);
      n = n < size - 1 ? n : size - 1;
      memcpy(out, value, n);
      out[n] = '\0';
      return true;
    }
  }
  return false;
}

static bool _credentials_ok(test_proxy const* p, char const* head)
{
  if (p->options.username == NULL)
  {
    return true;
  }
  char plain[512];
  int const plain_len
      = snprintf(plain, sizeof(plain), "%s:%s", p->options.username, p->options.password);
  unsigned char expected[1024];
  EVP_EncodeBlock(expected, (unsigned char const*)plain, plain_len);
  char got[1024];
  return _header(head, "Proxy-Authorization", got, sizeof(got)) && strncmp(got, "Basic ", 6) == 0
      && strcmp(got + 6, (char const*)expected) == 0;
}

static void _reply(test_proxy* p, int fd, int status, char const* reason)
{
  char reply[16384];
  int len = snprintf(reply, sizeof(reply), "HTTP/1.1 %d %s\r\n", status, reason);
  for (int padded = 0; padded < p->options.extra_header_bytes; padded += 98)
  {
    len += snprintf(reply + len, sizeof(reply) - (size_t)len, "X-Padding: %085d\r\n", padded);
  }
  len += snprintf(reply + len, sizeof(reply) - (size_t)len, "\r\n");
  if (p->options.reply_delay_ms > 0)
  {
    _sleep_ms(p->options.reply_delay_ms);
  }
  if (!p->options.fragment_reply)
  {
    (void)_send_all(fd, reply, (size_t)len);
    return;
  }
  for (int i = 0; i < len && !_stopping(p); i++)
  {
    (void)_send_all(fd, reply + i, 1);
    _sleep_ms(1);
  }
}

/** @brief Connect to "host:port" (host may be "[v6]"); -1 on failure. */
static int _connect_target(char const* authority)
{
  char host[256];
  char const* colon = strrchr(authority, ':');
  if (colon == NULL)
  {
    return -1;
  }
  size_t host_len = (size_t)(colon - authority);
  char const* h = authority;
  if (host_len >= 2 && h[0] == '[' && h[host_len - 1] == ']')
  {
    h++;
    host_len -= 2;
  }
  if (host_len >= sizeof(host))
  {
    return -1;
  }
  memcpy(host, h, host_len);
  host[host_len] = '\0';
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = NULL;
  if (getaddrinfo(host, colon + 1, &hints, &res) != 0)
  {
    return -1;
  }
  int fd = -1;
  for (struct addrinfo* a = res; a != NULL && fd < 0; a = a->ai_next)
  {
    fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) != 0)
    {
      close(fd);
      fd = -1;
    }
  }
  freeaddrinfo(res);
  return fd;
}

/** @brief Copy bytes both ways until either side closes or the proxy stops. */
static void _pump(test_proxy* p, int a, int b)
{
  char buf[4096];
  while (!_stopping(p))
  {
    struct pollfd pfd[2] = { { a, POLLIN, 0 }, { b, POLLIN, 0 } };
    int r = poll(pfd, 2, 50);
    if (r < 0)
    {
      return;
    }
    for (int i = 0; i < 2; i++)
    {
      if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))
      {
        ssize_t n = recv(pfd[i].fd, buf, sizeof(buf), 0);
        if (n <= 0 || !_send_all(pfd[1 - i].fd, buf, (size_t)n))
        {
          return;
        }
      }
    }
  }
}

static void _serve(test_proxy* p, int fd)
{
  char head[8192];
  if (!_read_request(p, fd, head, sizeof(head)))
  {
    return;
  }
  char* line_end = strstr(head, "\r\n");
  pthread_mutex_lock(&p->lock);
  p->requests++;
  size_t n = (size_t)(line_end - head);
  n = n < sizeof(p->last_request_line) - 1 ? n : sizeof(p->last_request_line) - 1;
  memcpy(p->last_request_line, head, n);
  p->last_request_line[n] = '\0';
  pthread_mutex_unlock(&p->lock);

  if (p->options.close_without_reply)
  {
    return;
  }
  if (p->options.garbage_reply)
  {
    (void)_send_all(fd, "SSH-2.0-test\r\n", 14);
    return;
  }
  if (!_credentials_ok(p, head))
  {
    pthread_mutex_lock(&p->lock);
    p->auth_failures++;
    pthread_mutex_unlock(&p->lock);
    _reply(p, fd, 407, "Proxy Authentication Required");
    return;
  }
  if (p->options.status != 0 && p->options.status != 200)
  {
    _reply(p, fd, p->options.status, "Refused");
    return;
  }
  char authority[300];
  if (sscanf(head, "CONNECT %299s HTTP/1.1", authority) != 1)
  {
    _reply(p, fd, 400, "Bad Request");
    return;
  }
  int target = _connect_target(authority);
  if (target < 0)
  {
    _reply(p, fd, 502, "Bad Gateway");
    return;
  }
  pthread_mutex_lock(&p->lock);
  p->tunnels++;
  pthread_mutex_unlock(&p->lock);
  _reply(p, fd, 200, "Connection established");
  _pump(p, fd, target);
  close(target);
}

static void* _run(void* arg)
{
  test_proxy* p = (test_proxy*)arg;
  while (!_stopping(p))
  {
    struct pollfd pfd = { p->listener, POLLIN, 0 };
    if (poll(&pfd, 1, 50) <= 0)
    {
      continue;
    }
    int fd = accept(p->listener, NULL, NULL);
    if (fd < 0)
    {
      continue;
    }
    _serve(p, fd);
    close(fd);
  }
  return NULL;
}

test_proxy* test_proxy_start(test_proxy_options const* options)
{
  test_proxy* p = (test_proxy*)calloc(1, sizeof(*p));
  if (p == NULL)
  {
    return NULL;
  }
  p->options = *options;
  pthread_mutex_init(&p->lock, NULL);
  p->listener = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(sa);
  if (p->listener < 0 || bind(p->listener, (struct sockaddr*)&sa, sizeof(sa)) != 0
      || listen(p->listener, 4) != 0 || getsockname(p->listener, (struct sockaddr*)&sa, &len) != 0
      || pthread_create(&p->thread, NULL, _run, p) != 0)
  {
    if (p->listener >= 0)
    {
      close(p->listener);
    }
    free(p);
    return NULL;
  }
  p->port = ntohs(sa.sin_port);
  return p;
}

uint16_t test_proxy_port(test_proxy const* p) { return p->port; }

void test_proxy_stop(test_proxy* p)
{
  if (p == NULL)
  {
    return;
  }
  pthread_mutex_lock(&p->lock);
  p->stop = true;
  pthread_mutex_unlock(&p->lock);
  pthread_join(p->thread, NULL);
  close(p->listener);
  pthread_mutex_destroy(&p->lock);
  free(p);
}

static int _get(test_proxy* p, int const* field)
{
  pthread_mutex_lock(&p->lock);
  int v = *field;
  pthread_mutex_unlock(&p->lock);
  return v;
}

int test_proxy_requests(test_proxy* p) { return _get(p, &p->requests); }
int test_proxy_tunnels(test_proxy* p) { return _get(p, &p->tunnels); }
int test_proxy_auth_failures(test_proxy* p) { return _get(p, &p->auth_failures); }

void test_proxy_last_request_line(test_proxy* p, char* buffer, size_t size)
{
  pthread_mutex_lock(&p->lock);
  snprintf(buffer, size, "%s", p->last_request_line);
  pthread_mutex_unlock(&p->lock);
}
