// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt5_sample_connect.c
 * @brief Sample: connect to an MQTT 5.0 broker over TCP, subscribe, publish, receive, disconnect.
 *
 * Settings: see az_mqtt_sample_common.h (default port 1883).
 *
 * All memory is caller storage: no allocation.
 */

#include <az_mqtt5/az_mqtt5_client.h>

#include "az_mqtt_sample_common.h"

#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Reinterpret any fixed-size array as a byte array to use AZ_SPAN_FROM_BUFFER.
#define SPAN_FROM_ARRAY(ARRAY) AZ_SPAN_FROM_BUFFER(*(uint8_t(*)[sizeof(ARRAY)])(ARRAY))

static uint8_t s_send_buffer[AZ_MQTT_SAMPLE_BUFFER_SIZE];
static uint8_t s_receive_buffer[AZ_MQTT_SAMPLE_BUFFER_SIZE];
static az_mqtt_sample_transport_storage s_transport_storage;
static az_mqtt_inflight_entry s_inflight[8];

// Where received MQTT 5.0 properties are decoded into.
static az_mqtt5_user_property s_connack_user_properties[8];
static az_mqtt5_user_property s_publish_user_properties[8];
static int32_t s_publish_subscription_ids[8];
static az_mqtt5_reason_code s_suback_reason_codes[8];
static az_mqtt5_user_property s_suback_user_properties[8];
static az_mqtt5_user_property s_ack_user_properties[8];
static az_mqtt5_user_property s_disconnect_user_properties[8];

#define TOPIC "az-mqtt-sample/mqtt5/hello"

static bool s_subscribed;
static bool s_acknowledged;
static bool s_received;

// ──────────────────────── Callbacks ──────────────────────────

static void on_connack(az_mqtt5_client* client, az_mqtt5_connack_data const* connack)
{
  (void)client;
  printf(
      "[CONNACK] reason=0x%02X session_present=%d\n",
      (unsigned)connack->reason_code,
      connack->session_present);
}

static void on_suback(az_mqtt5_client* client, az_mqtt5_suback_data const* suback)
{
  (void)client;
  printf("[SUBACK] packet_id=%u reason_codes=[", suback->packet_id);
  for (int32_t i = 0; i < suback->reason_code_count; i++)
  {
    printf("%s0x%02X", i > 0 ? ", " : "", (unsigned)suback->reason_codes[i]);
    // Reason codes 0x80 and above refuse the subscription.
    s_subscribed = suback->reason_codes[i] < AZ_MQTT5_REASON_UNSPECIFIED_ERROR;
  }
  printf("]\n");
}

static void on_puback(az_mqtt5_client* client, az_mqtt5_ack_data const* ack)
{
  (void)client;
  printf("[PUBACK] packet_id=%u reason=0x%02X\n", ack->packet_id, (unsigned)ack->reason_code);
  s_acknowledged
      = az_result_succeeded(ack->status) && ack->reason_code < AZ_MQTT5_REASON_UNSPECIFIED_ERROR;
}

static void on_publish(az_mqtt5_client* client, az_mqtt5_publish_data const* publish)
{
  (void)client;
  printf(
      "[PUBLISH received] topic=\"%.*s\" qos=%d payload=\"%.*s\"\n",
      az_span_size(publish->topic),
      (char const*)az_span_ptr(publish->topic),
      (int)publish->qos,
      az_span_size(publish->payload),
      (char const*)az_span_ptr(publish->payload));
  s_received = s_received || az_span_is_content_equal(publish->topic, AZ_SPAN_FROM_STR(TOPIC));
}

static void on_disconnect(az_mqtt5_client* client, az_mqtt5_disconnect_data const* disconnect)
{
  (void)client;
  printf("[DISCONNECT from broker] reason=0x%02X\n", (unsigned)disconnect->reason_code);
}

// ──────────────────────── Main ───────────────────────────────

int main(void)
{
  az_mqtt_sample_settings settings;
  if (!az_mqtt_sample_settings_read(&settings, 1883, "az-mqtt5-sample-connect"))
  {
    return 1;
  }
  printf(
      "Connecting to %.*s:%u\n",
      az_span_size(settings.host),
      (char const*)az_span_ptr(settings.host),
      settings.port);

  // The platform transport (TCP; TLS and proxies are shown in other samples).
  az_mqtt_transport* transport = (az_mqtt_transport*)s_transport_storage.bytes;
  if (az_mqtt_transport_sizeof() > (int32_t)sizeof(s_transport_storage))
  {
    printf("ERROR: transport storage too small (need %d bytes)\n", az_mqtt_transport_sizeof());
    return 1;
  }
  az_result rc = az_mqtt_transport_init(transport);
  if (az_result_failed(rc))
  {
    printf("ERROR: transport init failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  az_mqtt5_client_options options;
  memset(&options, 0, sizeof(options));
  options.transport = transport;
  options.hostname = settings.host;
  options.port = settings.port;
  options.send_buffer = AZ_SPAN_FROM_BUFFER(s_send_buffer);
  options.receive_buffer = AZ_SPAN_FROM_BUFFER(s_receive_buffer);
  options.inflight_control_buffer = SPAN_FROM_ARRAY(s_inflight);
  options.connect_options = az_mqtt5_connect_options_default();
  options.connect_options.client_id = settings.client_id;
  options.connect_options.username = settings.username;
  options.connect_options.password = settings.password;
  options.connect_options.keep_alive_seconds = 30;
  options.buffers.connack_user_properties = SPAN_FROM_ARRAY(s_connack_user_properties);
  options.buffers.publish_user_properties = SPAN_FROM_ARRAY(s_publish_user_properties);
  options.buffers.publish_subscription_identifiers = SPAN_FROM_ARRAY(s_publish_subscription_ids);
  options.buffers.suback_reason_codes = SPAN_FROM_ARRAY(s_suback_reason_codes);
  options.buffers.suback_user_properties = SPAN_FROM_ARRAY(s_suback_user_properties);
  options.buffers.ack_user_properties = SPAN_FROM_ARRAY(s_ack_user_properties);
  options.buffers.disconnect_user_properties = SPAN_FROM_ARRAY(s_disconnect_user_properties);
  options.on_connack = on_connack;
  options.on_suback = on_suback;
  options.on_puback = on_puback;
  options.on_publish = on_publish;
  options.on_disconnect = on_disconnect;

  az_mqtt5_client client;
  rc = az_mqtt5_client_init(&client, &options);
  if (az_result_failed(rc))
  {
    printf("ERROR: client init failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  // Blocks until the CONNACK, at most 10 s.
  rc = az_mqtt5_client_connect(&client, 10000);
  if (az_result_failed(rc))
  {
    printf("ERROR: connect failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  az_mqtt5_subscription subscription;
  memset(&subscription, 0, sizeof(subscription));
  subscription.topic_filter = AZ_SPAN_FROM_STR("az-mqtt-sample/mqtt5/#");
  subscription.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  uint16_t packet_id = 0;
  rc = az_mqtt5_client_subscribe(&client, &subscription, 1, &packet_id);
  printf("Subscribe sent (packet_id=%u): 0x%08X\n", packet_id, (unsigned)rc);

  az_mqtt5_publish_options message = az_mqtt5_publish_options_default();
  message.topic = AZ_SPAN_FROM_STR(TOPIC);
  message.payload = AZ_SPAN_FROM_STR("Hello from az_mqtt5_client");
  message.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  message.payload_format_indicator = 1; // UTF-8 text.
  if (az_result_succeeded(rc))
  {
    rc = az_mqtt5_client_publish(&client, &message, &packet_id);
    printf("Publish sent (packet_id=%u): 0x%08X\n", packet_id, (unsigned)rc);
  }

  // SUBACK, PUBACK and the message echoed back by the broker arrive here.
  for (int i = 0;
       i < 5 && az_result_succeeded(rc) && !(s_subscribed && s_acknowledged && s_received);
       i++)
  {
    rc = az_mqtt5_client_process_loop(&client, 1000);
  }
  if (az_result_failed(rc))
  {
    printf("ERROR: 0x%08X\n", (unsigned)rc);
  }
  else if (!(s_subscribed && s_acknowledged && s_received))
  {
    printf(
        "ERROR: subscribed=%d acknowledged=%d received=%d\n",
        s_subscribed,
        s_acknowledged,
        s_received);
  }

  az_result const disconnect_rc
      = az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION);
  printf("Disconnected: 0x%08X\n", (unsigned)disconnect_rc);
  return az_result_succeeded(rc) && s_subscribed && s_acknowledged && s_received
          && az_result_succeeded(disconnect_rc)
      ? 0
      : 1;
}
