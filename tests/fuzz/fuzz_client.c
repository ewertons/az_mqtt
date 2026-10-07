// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file fuzz_client.c
 * @brief libFuzzer target: the client's receive path (framing, every packet decoder, in-flight
 * state, resend on resume, callbacks), fed what the broker sends. Built per version
 * (AZ_MQTT_FUZZ_VERSION 3 or 5).
 *
 * Input: one options byte, then the bytes the broker sends.
 * - bits 0-1: receive chunk size (1, 7, 64, unlimited);
 * - bit 2: the session outlives the connection (clean session/start 0, inflight_message_buffer);
 * - bit 3: once connected, subscribe and publish QoS 0, 1 and 2;
 * - bit 4: on_publish publishes (callback re-entrancy);
 * - bit 5: over WebSockets: the broker bytes follow a valid upgrade reply. Otherwise reconnects,
 *   up to 3 times, while input remains.
 */

#include "fuzz_websocket.h"
#include "test_fake_transport.h"


#include <az_mqtt/az_mqtt_websocket.h>

#if AZ_MQTT_FUZZ_VERSION == 5
#include <az_mqtt5/az_mqtt5_client.h>
#define _CLIENT(name) az_mqtt5_##name
#else
#include <az_mqtt3/az_mqtt3_client.h>
#define _CLIENT(name) az_mqtt3_##name
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define _BUFFER_SIZE 1024
#define _SPAN_OF(ARRAY) az_span_create((uint8_t*)(ARRAY), (int32_t)sizeof(ARRAY))

int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size);

/** @brief Results the fuzzer has no use for (the input decides them). */
static void _ignore(az_result rc) { (void)rc; }

static uint8_t s_input[70000];
static uint8_t s_sent[70000];
static uint8_t s_send_buffer[_BUFFER_SIZE];
static uint8_t s_receive_buffer[_BUFFER_SIZE];
static uint8_t s_inflight_messages[2 * (_BUFFER_SIZE + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD)];
static az_mqtt_inflight_entry s_inflight[4];
static bool s_publish_from_callback;
#if AZ_MQTT_FUZZ_VERSION == 5
static az_mqtt5_user_property s_decode_user_properties[4];
static int32_t s_decode_codes[4];
#endif

static void _publish(_CLIENT(client) * client, az_mqtt_qos qos)
{
  _CLIENT(publish_options) message = _CLIENT(publish_options_default)();
  message.topic = AZ_SPAN_FROM_STR("t/out");
  message.payload = AZ_SPAN_FROM_STR("payload");
  message.qos = qos;
  _ignore(_CLIENT(client_publish)(client, &message, NULL));
}

static void _on_publish(_CLIENT(client) * client, _CLIENT(publish_data) const* publish)
{
  (void)publish;
  if (s_publish_from_callback)
  {
    _publish(client, AZ_MQTT_QOS_AT_LEAST_ONCE);
  }
}

int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
  if (size < 1 || size > 65536)
  {
    return 0;
  }
  uint8_t const flags = data[0];
  bool const keep_session = (flags & 4) != 0;
  bool const exchange = (flags & 8) != 0;
#ifndef AZ_MQTT_NO_WEBSOCKETS
  bool const websocket = (flags & 32) != 0;
#else
  bool const websocket = false; // Not built: bit 5 is ignored.
#endif
  s_publish_from_callback = (flags & 16) != 0;
  static int32_t const chunks[] = { 1, 7, 64, INT32_MAX };

  memset(s_sent, 0, sizeof(s_sent));
  static test_fake_transport fake;
  test_fake_transport_init(
      &fake, AZ_SPAN_FROM_BUFFER(s_input), az_span_create(s_sent, (int32_t)sizeof(s_sent) - 1));
  fake.chunk = chunks[flags & 3];
  // Until the upgrade reply is fed, an empty input means "nothing yet".
  fake.end_of_input = websocket ? AZ_OK : AZ_MQTT_ERROR_CONNECTION_CLOSED;
  az_mqtt_transport* transport = &fake.layer.base;
#ifndef AZ_MQTT_NO_WEBSOCKETS
  static az_mqtt_websocket ws;
  if (websocket)
  {
    az_mqtt_websocket_options const ws_options = az_mqtt_websocket_options_default();
    if (az_result_failed(az_mqtt_websocket_init(&ws, transport, &ws_options)))
    {
      return 0;
    }
    transport = az_mqtt_websocket_get_transport(&ws);
  }
  else
#endif
  {
    test_fake_transport_feed(&fake, data + 1, (int32_t)size - 1);
  }

  _CLIENT(client_options) options;
  memset(&options, 0, sizeof(options));
  options.transport = transport;
  options.hostname = AZ_SPAN_FROM_STR("broker");
  options.port = 1883;
  options.send_buffer = AZ_SPAN_FROM_BUFFER(s_send_buffer);
  options.receive_buffer = AZ_SPAN_FROM_BUFFER(s_receive_buffer);
  options.inflight_control_buffer = _SPAN_OF(s_inflight);
  options.connect_options = _CLIENT(connect_options_default)();
  options.connect_options.client_id = AZ_SPAN_FROM_STR("fuzz");
  options.on_publish = _on_publish;
#if AZ_MQTT_FUZZ_VERSION == 5
  options.connect_options.clean_start = !keep_session;
  options.connect_options.session_expiry_interval = keep_session ? 60 : 0;
  options.decode_user_properties = _SPAN_OF(s_decode_user_properties);
  options.decode_codes = _SPAN_OF(s_decode_codes);
#else
  options.connect_options.clean_session = !keep_session;
#endif
  if (keep_session)
  {
    options.inflight_message_buffer = AZ_SPAN_FROM_BUFFER(s_inflight_messages);
  }

  static _CLIENT(client) client;
  if (az_result_failed(_CLIENT(client_init)(&client, &options)))
  {
    return 0;
  }

#ifndef AZ_MQTT_NO_WEBSOCKETS
  bool upgrade_fed = !websocket;
#endif
  for (int connects = 0; connects < (websocket ? 1 : 4); connects++)
  {
    if (az_result_failed(_CLIENT(client_connect_start)(&client, -1)))
    {
      break;
    }
    bool exchanged = false;
    for (int i = 0; i < 100000; i++)
    {
#ifndef AZ_MQTT_NO_WEBSOCKETS
      if (!upgrade_fed && fuzz_feed_upgrade_reply(&fake, s_sent))
      {
        test_fake_transport_feed(&fake, data + 1, (int32_t)size - 1);
        fake.end_of_input = AZ_MQTT_ERROR_CONNECTION_CLOSED;
        upgrade_fed = true;
      }
#endif
      if (exchange && !exchanged
          && _CLIENT(client_get_state)(&client) == AZ_MQTT_CLIENT_STATE_CONNECTED)
      {
        exchanged = true;
        _CLIENT(subscription) subscription;
        memset(&subscription, 0, sizeof(subscription));
        subscription.topic_filter = AZ_SPAN_FROM_STR("t/#");
        subscription.qos = AZ_MQTT_QOS_EXACTLY_ONCE;
        _ignore(_CLIENT(client_subscribe)(&client, &subscription, 1, NULL));
        _publish(&client, AZ_MQTT_QOS_AT_MOST_ONCE);
        _publish(&client, AZ_MQTT_QOS_AT_LEAST_ONCE);
        _publish(&client, AZ_MQTT_QOS_EXACTLY_ONCE);
      }
      if (az_result_failed(_CLIENT(client_process_loop)(&client, 0)))
      {
        break; // The session ended.
      }
    }
    if (fake.input_read >= fake.input_size)
    {
      break;
    }
  }
#if AZ_MQTT_FUZZ_VERSION == 5
  _ignore(az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION));
#else
  _ignore(az_mqtt3_client_disconnect(&client));
#endif
  return 0;
}
