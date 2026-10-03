// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_fake_transport.h
 * @brief In-memory az_mqtt_transport for tests: scripted input, recorded output, injected
 * failures. Portable; no I/O.
 */

#ifndef TEST_FAKE_TRANSPORT_H
#define TEST_FAKE_TRANSPORT_H

#include "az_mqtt_io_layers_internal.h"

#include <az_mqtt/az_mqtt_transport.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
  _az_mqtt_io_layer layer; ///< Must be first; layer.base is the transport.
  /** @brief What the peer sends (fed with test_fake_transport_feed()); read up to chunk at a time.
   */
  az_span input;
  int32_t input_size;
  int32_t input_read;
  int32_t chunk;
  /** @brief Everything sent, in order (truncated to its size; sent_size counts all). */
  az_span sent;
  int32_t sent_size;
  int send_calls;
  /** @brief Bytes send_some() may still take (-1: no limit), used up as it takes them: buffer space. */
  int32_t send_some_budget;
  /** @brief Fail this send call (1-based; 0: none) and later ones. */
  int fail_send_call;
  /** @brief connect_poll() returns AZ_MQTT_ERROR_TIMEOUT this many times before succeeding. */
  int connect_polls_pending;
  /** @brief Returned by receive once the input is exhausted; AZ_OK: no bytes. */
  az_result end_of_input;
  /** @brief Non-zero: a failing send or receive first reports this socket error (as a socket does). */
  int32_t failure_errno;
  int connects;
  /** @brief Of the last connect_start(). */
  az_span host;
  uint16_t port;
  int shutdowns;
  int closes;
  bool connected;
  az_mqtt_tls_options const* tls_options;
  az_mqtt_proxy_options const* proxy;
  az_mqtt_transport_error_fn error_callback;
  void* error_context;
} test_fake_transport;

/** @brief Initialize over caller buffers for the input and for what is sent. */
void test_fake_transport_init(test_fake_transport* fake, az_span input, az_span sent);

/** @brief Append @p size bytes to the input. */
void test_fake_transport_feed(test_fake_transport* fake, void const* data, int32_t size);

#endif // TEST_FAKE_TRANSPORT_H
