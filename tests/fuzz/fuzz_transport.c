// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file fuzz_transport.c
 * @brief libFuzzer target: the transport layers' parsers of what the network sends, fed through
 * the layers themselves over an in-memory transport.
 *
 * Input: one options byte, then the bytes received.
 * - bits 0-1: receive chunk size (1, 7, 64, unlimited);
 * - bit 2: WebSocket layer (upgrade reply, then frames); else HTTP CONNECT proxy layer (CONNECT
 *   reply, then tunnelled bytes);
 * - bit 3 (WebSocket): the input follows a valid upgrade reply, so it reaches the frames (the key
 *   in each request is random).
 * After a completed handshake, the rest is received until the layer fails (at the input's end).
 */

#include "az_mqtt_io_layers_internal.h"
#include "fuzz_websocket.h"
#include "test_fake_transport.h"

#include <az_mqtt/az_mqtt_websocket.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size);

static uint8_t s_input[70000];
static uint8_t s_sent[8192];
static uint8_t s_receive[512];

int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
  if (size < 1 || size > 65536)
  {
    return 0;
  }
  static int32_t const chunks[] = { 1, 7, 64, INT32_MAX };
  static test_fake_transport fake;
  test_fake_transport_init(
      &fake, AZ_SPAN_FROM_BUFFER(s_input), az_span_create(s_sent, (int32_t)sizeof(s_sent) - 1));
  fake.chunk = chunks[data[0] & 3];
  bool const websocket = (data[0] & 4) != 0;
  bool upgrade_fed = !(websocket && (data[0] & 8) != 0);
  memset(s_sent, 0, sizeof(s_sent));
  if (upgrade_fed)
  {
    fake.end_of_input = AZ_MQTT_ERROR_CONNECTION_CLOSED;
    test_fake_transport_feed(&fake, data + 1, (int32_t)size - 1);
  }
  else
  {
    fake.end_of_input = AZ_OK; // Nothing yet, until the reply is fed.
  }

  // A layer that is not built: nothing to fuzz.
  az_mqtt_transport* t;
  if (websocket)
  {
#ifndef AZ_MQTT_NO_WEBSOCKETS
    static az_mqtt_websocket ws;
    az_mqtt_websocket_options const options = az_mqtt_websocket_options_default();
    if (az_result_failed(az_mqtt_websocket_init(&ws, &fake.layer.base, &options)))
    {
      return 0;
    }
    t = az_mqtt_websocket_get_transport(&ws);
#else
    return 0;
#endif
  }
  else
  {
#ifndef AZ_MQTT_NO_PROXY
    static _az_mqtt_proxy_transport proxy;
    static az_mqtt_proxy_options proxy_options;
    if (az_result_failed(_az_mqtt_proxy_transport_init(&proxy, &fake.layer)))
    {
      return 0;
    }
    t = &proxy.layer.base;
    memset(&proxy_options, 0, sizeof(proxy_options));
    proxy_options.host = AZ_SPAN_FROM_STR("proxy");
    proxy_options.port = 3128;
    proxy_options.username = AZ_SPAN_FROM_STR("user");
    proxy_options.password = AZ_SPAN_FROM_STR("pass");
    if (az_result_failed(az_mqtt_transport_set_proxy(t, &proxy_options)))
    {
      return 0;
    }
#else
    return 0;
#endif
  }

  az_result rc = az_mqtt_transport_connect_start(t, AZ_SPAN_FROM_STR("broker"), 1883, NULL);
  for (int i = 0; i < 100000 && rc == AZ_OK; i++)
  {
#ifndef AZ_MQTT_NO_WEBSOCKETS
    if (!upgrade_fed && fuzz_feed_upgrade_reply(&fake, s_sent))
    {
      test_fake_transport_feed(&fake, data + 1, (int32_t)size - 1);
      fake.end_of_input = AZ_MQTT_ERROR_CONNECTION_CLOSED;
      upgrade_fed = true;
    }
#endif
    rc = az_mqtt_transport_connect_poll(t, 0);
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      rc = AZ_OK; // In progress.
    }
    else
    {
      break;
    }
  }
  // Until the layer fails: the input ends with AZ_MQTT_ERROR_CONNECTION_CLOSED once the bytes a
  // handshake read ahead have been received too.
  for (int i = 0; i < 100000 && rc == AZ_OK; i++)
  {
    az_span received;
    rc = az_mqtt_transport_receive(t, AZ_SPAN_FROM_BUFFER(s_receive), 0, &received);
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      rc = AZ_OK;
    }
  }
  az_mqtt_transport_close(t);
  return 0;
}
