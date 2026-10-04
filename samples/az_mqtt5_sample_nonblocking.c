// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt5_sample_nonblocking.c
 * @brief Sample: drive an MQTT 5.0 client from an application loop that never blocks for long:
 * connect with connect_start(), then let process_loop() progress the connect, sends and receives
 * while the loop does its own work. Publishes three QoS 1 messages and waits for their PUBACKs.
 *
 * Settings: see az_mqtt_sample_common.h (default port 1883).
 */

#include <az_mqtt5/az_mqtt5_client.h>

#include "az_mqtt_sample_common.h"

#include <azure/core/az_span.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SPAN_FROM_ARRAY(ARRAY) AZ_SPAN_FROM_BUFFER(*(uint8_t(*)[sizeof(ARRAY)])(ARRAY))

#define MESSAGE_COUNT 3

static uint8_t s_send_buffer[AZ_MQTT_SAMPLE_BUFFER_SIZE];
static uint8_t s_receive_buffer[AZ_MQTT_SAMPLE_BUFFER_SIZE];
static az_mqtt_sample_transport_storage s_transport_storage;
static az_mqtt_inflight_entry s_inflight[8];
static az_mqtt5_user_property s_ack_user_properties[8];

static int s_acknowledged;

static void on_connack(az_mqtt5_client* client, az_mqtt5_connack_data const* connack)
{
  (void)client;
  printf("[CONNACK] reason=0x%02X\n", (unsigned)connack->reason_code);
}

static void on_puback(az_mqtt5_client* client, az_mqtt5_ack_data const* ack)
{
  (void)client;
  printf("[PUBACK] packet_id=%u reason=0x%02X\n", ack->packet_id, (unsigned)ack->reason_code);
  s_acknowledged++;
}

// Called whenever the session ends: after disconnect() (AZ_OK) or on a failure.
static void on_connection_closed(az_mqtt5_client* client, az_result reason)
{
  (void)client;
  printf("[CLOSED] reason=0x%08X\n", (unsigned)reason);
}

int main(void)
{
  az_mqtt_sample_settings settings;
  if (!az_mqtt_sample_settings_read(&settings, 1883, "az-mqtt5-sample-nonblocking"))
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
  options.inflight_control_buffer = SPAN_FROM_ARRAY(s_inflight);
  options.connect_options = az_mqtt5_connect_options_default();
  options.connect_options.client_id = settings.client_id;
  options.connect_options.username = settings.username;
  options.connect_options.password = settings.password;
  options.connect_options.keep_alive_seconds = 30;
  options.buffers.ack_user_properties = SPAN_FROM_ARRAY(s_ack_user_properties);
  options.on_connack = on_connack;
  options.on_puback = on_puback;
  options.on_connection_closed = on_connection_closed;

  az_mqtt5_client client;
  rc = az_mqtt5_client_init(&client, &options);
  if (az_result_failed(rc))
  {
    printf("ERROR: client init failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  // Returns once the TCP connect has started (only name resolution may block). The whole connect
  // must complete within 10 s.
  rc = az_mqtt5_client_connect_start(&client, 10000);
  if (az_result_failed(rc))
  {
    printf("ERROR: connect_start failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  // The application loop: each iteration waits at most 100 ms in process_loop().
  int published = 0;
  int iterations = 0;
  while (az_result_succeeded(rc) && s_acknowledged < MESSAGE_COUNT && iterations < 100)
  {
    iterations++;
    rc = az_mqtt5_client_process_loop(&client, 100);
    if (az_result_failed(rc)
        || az_mqtt5_client_get_state(&client) != AZ_MQTT_CLIENT_STATE_CONNECTED)
    {
      continue; // Failed (on_connection_closed reported it), or still connecting.
    }

    if (published < MESSAGE_COUNT)
    {
      char payload[32];
      int const size = snprintf(payload, sizeof(payload), "message %d", published + 1);
      az_mqtt5_publish_options message = az_mqtt5_publish_options_default();
      message.topic = AZ_SPAN_FROM_STR("az-mqtt-sample/mqtt5/nonblocking");
      message.payload = az_span_create((uint8_t*)payload, size);
      message.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
      uint16_t packet_id = 0;
      // Sent now (the payload is not kept); its PUBACK arrives in a later process_loop().
      rc = az_mqtt5_client_publish(&client, &message, &packet_id);
      printf("Publish %d sent (packet_id=%u): 0x%08X\n", published + 1, packet_id, (unsigned)rc);
      published++;
    }
    // ... the application's own work goes here ...
  }
  printf("%d iterations, %d PUBACK(s)\n", iterations, s_acknowledged);

  az_result const disconnect_rc
      = az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION);
  return s_acknowledged == MESSAGE_COUNT && az_result_succeeded(disconnect_rc) ? 0 : 1;
}
