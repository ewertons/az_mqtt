// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_proxy.h
 * @brief In-process HTTP CONNECT proxy for tests (POSIX). One connection at a time.
 */
#ifndef TEST_PROXY_H
#define TEST_PROXY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
  /** @brief Required Basic credentials; NULL: none required. */
  char const* username;
  char const* password;
  /** @brief Status of the reply to an acceptable request; 0: 200 (open the tunnel). */
  int status;
  /** @brief Send the reply one byte at a time. */
  bool fragment_reply;
  /** @brief Approximate bytes of extra header lines in the reply. */
  int extra_header_bytes;
  /** @brief Wait before replying. */
  int reply_delay_ms;
  /** @brief Close the connection instead of replying. */
  bool close_without_reply;
  /** @brief Reply with something that is not HTTP. */
  bool garbage_reply;
} test_proxy_options;

typedef struct test_proxy test_proxy;

test_proxy* test_proxy_start(test_proxy_options const* options);
uint16_t test_proxy_port(test_proxy const* proxy);
void test_proxy_stop(test_proxy* proxy);

/** @brief Requests received, tunnels opened, and requests refused with 407. */
int test_proxy_requests(test_proxy* proxy);
int test_proxy_tunnels(test_proxy* proxy);
int test_proxy_auth_failures(test_proxy* proxy);

/** @brief The last request line (e.g. "CONNECT host:1883 HTTP/1.1"); "" if none. */
void test_proxy_last_request_line(test_proxy* proxy, char* buffer, size_t size);

#endif // TEST_PROXY_H
