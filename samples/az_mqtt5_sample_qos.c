// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt5_sample_qos.c
 * @brief Sample: MQTT 5.0 QoS 0, 1 and 2. Subscribes at QoS 2, publishes one message at each
 * QoS and receives them back:
 * - QoS 0: no acknowledgement;
 * - QoS 1: PUBLISH, PUBACK (on_puback);
 * - QoS 2: PUBLISH, PUBREC, PUBREL, PUBCOMP (on_pubcomp), both ways. The client sends PUBREL and
 *   PUBREC/PUBCOMP itself.
 *
 * Settings: see az_mqtt_sample_common.h (default port 1883).
 */

#include <az_mqtt5/az_mqtt5_client.h>

#include "az_mqtt_sample_common.h"

#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SPAN_FROM_ARRAY(ARRAY) AZ_SPAN_FROM_BUFFER(*(uint8_t(*)[sizeof(ARRAY)])(ARRAY))

#define TOPIC_FILTER "az-mqtt-sample/mqtt5/qos/#"

static uint8_t s_send_buffer[AZ_MQTT_SAMPLE_BUFFER_SIZE];
static uint8_t s_receive_buffer[AZ_MQTT_SAMPLE_BUFFER_SIZE];
static az_mqtt_sample_transport_storage s_transport_storage;
static az_mqtt_inflight_entry s_inflight[8];
static az_mqtt5_reason_code s_suback_reason_codes[8];
static az_mqtt5_user_property s_ack_user_properties[8];

static bool s_subscribed;
static uint16_t s_qos1_packet_id;
static uint16_t s_qos2_packet_id;
static bool s_qos1_acknowledged;
static bool s_qos2_completed;
static bool s_incoming_qos2_completed;
static bool s_received[3]; // By QoS.

static void on_suback(az_mqtt5_client* client, az_mqtt5_suback_data const* suback)
{
  (void)client;
  az_mqtt5_reason_code const code
      = suback->reason_code_count > 0 ? suback->reason_codes[0] : AZ_MQTT5_REASON_UNSPECIFIED_ERROR;
  printf("[SUBACK] reason=0x%02X\n", (unsigned)code);
  s_subscribed = code == AZ_MQTT5_REASON_GRANTED_QOS_2;
}

static void on_puback(az_mqtt5_client* client, az_mqtt5_ack_data const* ack)
{
  (void)client;
  printf(
      "[PUBACK] packet_id=%u reason=0x%02X status=0x%08X\n",
      ack->packet_id,
      (unsigned)ack->reason_code,
      (unsigned)ack->status);
  s_qos1_acknowledged = ack->packet_id == s_qos1_packet_id && az_result_succeeded(ack->status)
      && ack->reason_code < AZ_MQTT5_REASON_UNSPECIFIED_ERROR;
}

// Outgoing QoS 2: PUBCOMP received, or a PUBREC with a reason code of 0x80 or more (rejected).
// Incoming QoS 2: PUBREL received and PUBCOMP sent.
static void on_pubcomp(az_mqtt5_client* client, az_mqtt5_ack_data const* ack)
{
  (void)client;
  printf(
      "[PUBCOMP] packet_id=%u reason=0x%02X status=0x%08X\n",
      ack->packet_id,
      (unsigned)ack->reason_code,
      (unsigned)ack->status);
  if (ack->packet_id == s_qos2_packet_id)
  {
    s_qos2_completed
        = az_result_succeeded(ack->status) && ack->reason_code < AZ_MQTT5_REASON_UNSPECIFIED_ERROR;
  }
  else
  {
    s_incoming_qos2_completed = true;
  }
}

static void on_publish(az_mqtt5_client* client, az_mqtt5_publish_data const* publish)
{
  (void)client;
  printf(
      "[PUBLISH received] topic=\"%.*s\" qos=%d packet_id=%u\n",
      az_span_size(publish->topic),
      (char const*)az_span_ptr(publish->topic),
      (int)publish->qos,
      publish->packet_id);
  s_received[publish->qos] = true;
}

int main(void)
{
  az_mqtt_sample_settings settings;
  if (!az_mqtt_sample_settings_read(&settings, 1883, "az-mqtt5-sample-qos"))
  {
    return 1;
  }
  printf(
      "Connecting to %.*s:%u\n",
      az_span_size(settings.host),
      (char const*)az_span_ptr(settings.host),
      settings.port);

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
  // QoS 1/2 exchanges in flight, both ways, until their last acknowledgement.
  options.inflight_control_buffer = SPAN_FROM_ARRAY(s_inflight);
  options.connect_options = az_mqtt5_connect_options_default();
  options.connect_options.client_id = settings.client_id;
  options.connect_options.username = settings.username;
  options.connect_options.password = settings.password;
  options.buffers.suback_reason_codes = SPAN_FROM_ARRAY(s_suback_reason_codes);
  options.buffers.ack_user_properties = SPAN_FROM_ARRAY(s_ack_user_properties);
  options.on_suback = on_suback;
  options.on_puback = on_puback;
  options.on_pubcomp = on_pubcomp;
  options.on_publish = on_publish;

  az_mqtt5_client client;
  rc = az_mqtt5_client_init(&client, &options);
  if (az_result_failed(rc))
  {
    printf("ERROR: client init failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  rc = az_mqtt5_client_connect(&client, 10000);
  if (az_result_failed(rc))
  {
    printf("ERROR: connect failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  // Each message is delivered at the lower of its PUBLISH QoS and the subscription QoS.
  az_mqtt5_subscription subscription;
  memset(&subscription, 0, sizeof(subscription));
  subscription.topic_filter = AZ_SPAN_FROM_STR(TOPIC_FILTER);
  subscription.qos = AZ_MQTT_QOS_EXACTLY_ONCE;
  rc = az_mqtt5_client_subscribe(&client, &subscription, 1, NULL);
  for (int i = 0; i < 5 && az_result_succeeded(rc) && !s_subscribed; i++)
  {
    rc = az_mqtt5_client_process_loop(&client, 1000);
  }

  if (az_result_succeeded(rc) && s_subscribed)
  {
    az_mqtt5_publish_options message = az_mqtt5_publish_options_default();
    message.payload = AZ_SPAN_FROM_STR("hello");

    message.topic = AZ_SPAN_FROM_STR("az-mqtt-sample/mqtt5/qos/0");
    message.qos = AZ_MQTT_QOS_AT_MOST_ONCE;
    rc = az_mqtt5_client_publish(&client, &message, NULL);
    printf("QoS 0 publish sent: 0x%08X\n", (unsigned)rc);

    if (az_result_succeeded(rc))
    {
      message.topic = AZ_SPAN_FROM_STR("az-mqtt-sample/mqtt5/qos/1");
      message.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
      rc = az_mqtt5_client_publish(&client, &message, &s_qos1_packet_id);
      printf("QoS 1 publish sent (packet_id=%u): 0x%08X\n", s_qos1_packet_id, (unsigned)rc);
    }

    if (az_result_succeeded(rc))
    {
      message.topic = AZ_SPAN_FROM_STR("az-mqtt-sample/mqtt5/qos/2");
      message.qos = AZ_MQTT_QOS_EXACTLY_ONCE;
      rc = az_mqtt5_client_publish(&client, &message, &s_qos2_packet_id);
      printf("QoS 2 publish sent (packet_id=%u): 0x%08X\n", s_qos2_packet_id, (unsigned)rc);
    }
  }

  // Each process_loop() returns once it has handled what arrived: up to 10 s in all.
  bool done = false;
  for (int i = 0; i < 20 && az_result_succeeded(rc) && s_subscribed && !done; i++)
  {
    rc = az_mqtt5_client_process_loop(&client, 500);
    done = s_qos1_acknowledged && s_qos2_completed && s_incoming_qos2_completed && s_received[0]
        && s_received[1] && s_received[2];
  }
  if (az_result_failed(rc))
  {
    printf("ERROR: 0x%08X\n", (unsigned)rc);
  }
  else if (!done)
  {
    printf(
        "ERROR: subscribed=%d puback=%d pubcomp=%d incoming_pubcomp=%d received=%d,%d,%d\n",
        s_subscribed,
        s_qos1_acknowledged,
        s_qos2_completed,
        s_incoming_qos2_completed,
        s_received[0],
        s_received[1],
        s_received[2]);
  }

  az_result const disconnect_rc
      = az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION);
  printf("Disconnected: 0x%08X\n", (unsigned)disconnect_rc);
  return az_result_succeeded(rc) && done && az_result_succeeded(disconnect_rc) ? 0 : 1;
}
