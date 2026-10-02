// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file e2e_proxy.h
 * @brief Minimal in-process HTTP CONNECT proxy for the end-to-end tests (POSIX and Windows).
 * One connection at a time; tunnels to the host:port named in the request.
 */
#ifndef AZ_MQTT_E2E_PROXY_H
#define AZ_MQTT_E2E_PROXY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
  /** @brief Wait before replying. */
  int reply_delay_ms;
  /** @brief Send the reply one byte at a time, @p byte_delay_ms apart. */
  bool fragment_reply;
  int byte_delay_ms;
  /** @brief Reply 407 instead of opening the tunnel. */
  bool refuse_auth;
} e2e_proxy_options;

typedef struct e2e_proxy e2e_proxy;

/** @brief Start listening on 127.0.0.1 (an ephemeral port); NULL on failure. */
e2e_proxy* e2e_proxy_start(e2e_proxy_options const* options);
uint16_t e2e_proxy_port(e2e_proxy const* proxy);
/** @brief Tunnels opened so far. */
int e2e_proxy_tunnels(e2e_proxy* proxy);
void e2e_proxy_stop(e2e_proxy* proxy);

#endif // AZ_MQTT_E2E_PROXY_H
