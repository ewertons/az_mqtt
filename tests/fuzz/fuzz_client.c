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

#include "test_fake_transport.h"

#include "az_mqtt_websocket_internal.h"

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
static az_mqtt5_user_property s_properties[5][4];
static int32_t s_subscription_ids[4];
static az_mqtt5_reason_code s_reason_codes[4];
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

/** @brief Once the upgrade request has been sent, feed a valid reply to it. */
static bool _feed_upgrade_reply(test_fake_transport* fake)
{
  char const* key = strstr((char const*)s_sent, "\r\nSec-WebSocket-Key: ");
  if (key == NULL || strstr((char const*)s_sent, "\r\n\r\n") == NULL)
  {
    return false;
  }
  key += 21;
  static char const guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t joined[24 + sizeof(guid) - 1];
  memcpy(joined, key, 24);
  memcpy(joined + 24, guid, sizeof(guid) - 1);
  uint8_t digest[21] = { 0 };
  _az_mqtt_sha1(joined, sizeof(joined), digest);
  static char const b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char accept[28];
  for (int i = 0; i < 7; i++)
  {
    uint32_t const v = (uint32_t)digest[3 * i] << 16 | (uint32_t)digest[3 * i + 1] << 8
        | (uint32_t)digest[3 * i + 2];
    accept[4 * i] = b64[v >> 18 & 63];
    accept[4 * i + 1] = b64[v >> 12 & 63];
    accept[4 * i + 2] = b64[v >> 6 & 63];
    accept[4 * i + 3] = b64[v & 63];
  }
  accept[27] = '='; // 20 bytes: the last group holds 2.
  static char const head[]
      = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
  test_fake_transport_feed(fake, head, (int32_t)sizeof(head) - 1);
  test_fake_transport_feed(fake, accept, 28);
  test_fake_transport_feed(fake, "\r\n\r\n", 4);
  return true;
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
  bool const websocket = (flags & 32) != 0;
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
  options.buffers.connack_user_properties = _SPAN_OF(s_properties[0]);
  options.buffers.publish_user_properties = _SPAN_OF(s_properties[1]);
  options.buffers.suback_user_properties = _SPAN_OF(s_properties[2]);
  options.buffers.ack_user_properties = _SPAN_OF(s_properties[3]);
  options.buffers.disconnect_user_properties = _SPAN_OF(s_properties[4]);
  options.buffers.publish_subscription_identifiers = _SPAN_OF(s_subscription_ids);
  options.buffers.suback_reason_codes = _SPAN_OF(s_reason_codes);
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

  bool upgrade_fed = !websocket;
  for (int connects = 0; connects < (websocket ? 1 : 4); connects++)
  {
    if (az_result_failed(_CLIENT(client_connect_start)(&client, -1)))
    {
      break;
    }
    bool exchanged = false;
    for (int i = 0; i < 100000; i++)
    {
      if (!upgrade_fed && _feed_upgrade_reply(&fake))
      {
        test_fake_transport_feed(&fake, data + 1, (int32_t)size - 1);
        fake.end_of_input = AZ_MQTT_ERROR_CONNECTION_CLOSED;
        upgrade_fed = true;
      }
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
