// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt3_sample_websocket.c
 * @brief Sample: MQTT 3.1.1 over WebSockets (ws, or wss with AZ_MQTT_SAMPLE_TLS=1), optionally
 * through an HTTP CONNECT proxy. Publishes one QoS 1 message and waits for its PUBACK.
 *
 * Settings: see az_mqtt_sample_common.h (default port 80: set AZ_MQTT_SAMPLE_PORT for wss, e.g.
 * 443). AZ_MQTT_SAMPLE_WEBSOCKET_PATH is the request path (default /mqtt).
 */

#include <az_mqtt/az_mqtt_websocket.h>
#include <az_mqtt3/az_mqtt3_client.h>

#include "az_mqtt_sample_common.h"

#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t s_send_buffer[AZ_MQTT_SAMPLE_BUFFER_SIZE];
static uint8_t s_receive_buffer[AZ_MQTT_SAMPLE_BUFFER_SIZE];
static az_mqtt_sample_transport_storage s_transport_storage;
static az_mqtt_websocket s_websocket;
static az_mqtt_inflight_entry s_inflight[8];

static bool s_puback_received;
static bool s_acknowledged;

static void on_connack(az_mqtt3_client* client, az_mqtt3_connack_data const* connack)
{
  (void)client;
  printf("[CONNACK] return_code=%d\n", (int)connack->return_code);
}

static void on_puback(az_mqtt3_client* client, az_mqtt3_ack_data const* ack)
{
  (void)client;
  printf("[PUBACK] packet_id=%u status=0x%08X\n", ack->packet_id, (unsigned)ack->status);
  s_puback_received = true;
  s_acknowledged = az_result_succeeded(ack->status);
}

// Each platform error behind a failure, e.g. an X.509 verification error or a proxy's HTTP status.
// Also one per address given up for the next (e.g. ::1 refused, then 127.0.0.1 connects).
static void on_transport_error(az_mqtt3_client* client, az_mqtt_native_error const* error)
{
  (void)client;
  printf(
      "[transport] source=%d code=%ld result=0x%08X\n",
      (int)error->source,
      (long)error->code,
      (unsigned)error->result);
}

int main(void)
{
  az_mqtt_sample_settings settings;
  if (!az_mqtt_sample_settings_read(&settings, 80, "az-mqtt3-sample-websocket"))
  {
    return 1;
  }
  printf(
      "Connecting to %s://%.*s:%u%s\n",
      settings.tls ? "wss" : "ws",
      az_span_size(settings.host),
      (char const*)az_span_ptr(settings.host),
      settings.port,
      az_span_size(settings.proxy_host) > 0 ? " through a proxy" : "");

  // The platform transport carries the WebSocket: TCP, the proxy tunnel and TLS are its own.
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

  // The WebSocket layer over it; the client uses the layer as its transport.
  az_mqtt_websocket_options websocket_options = az_mqtt_websocket_options_default();
  websocket_options.path = settings.websocket_path;
  rc = az_mqtt_websocket_init(&s_websocket, transport, &websocket_options);
  if (az_result_failed(rc))
  {
    printf("ERROR: WebSocket init failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  az_mqtt_tls_options tls = az_mqtt_tls_options_default();
  tls.ca_cert_path = settings.ca_cert_path;
  tls.client_cert_path = settings.client_cert_path;
  tls.client_key_path = settings.client_key_path;

  az_mqtt_proxy_options proxy;
  memset(&proxy, 0, sizeof(proxy));
  proxy.host = settings.proxy_host;
  proxy.port = settings.proxy_port;
  proxy.username = settings.proxy_username;
  proxy.password = settings.proxy_password;

  az_mqtt3_client_options options;
  memset(&options, 0, sizeof(options));
  options.transport = az_mqtt_websocket_get_transport(&s_websocket);
  options.hostname = settings.host;
  options.port = settings.port;
  options.tls_options = settings.tls ? &tls : NULL;
  options.proxy_options = az_span_size(proxy.host) > 0 ? &proxy : NULL;
  options.send_buffer = AZ_SPAN_FROM_BUFFER(s_send_buffer);
  options.receive_buffer = AZ_SPAN_FROM_BUFFER(s_receive_buffer);
  options.inflight_control_buffer
      = az_span_create((uint8_t*)s_inflight, (int32_t)sizeof(s_inflight));
  options.connect_options = az_mqtt3_connect_options_default();
  options.connect_options.client_id = settings.client_id;
  options.connect_options.username = settings.username;
  options.connect_options.password = settings.password;
  options.on_connack = on_connack;
  options.on_puback = on_puback;
  options.on_transport_error = on_transport_error;

  az_mqtt3_client client;
  rc = az_mqtt3_client_init(&client, &options);
  if (az_result_failed(rc))
  {
    printf("ERROR: client init failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  rc = az_mqtt3_client_connect(&client, 10000);
  if (az_result_failed(rc))
  {
    printf("ERROR: connect failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  az_mqtt3_publish_options message = az_mqtt3_publish_options_default();
  message.topic = AZ_SPAN_FROM_STR("az-mqtt-sample/mqtt3/websocket");
  message.payload = AZ_SPAN_FROM_STR("Hello over WebSockets");
  message.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  uint16_t packet_id = 0;
  rc = az_mqtt3_client_publish(&client, &message, &packet_id);
  printf("Publish sent (packet_id=%u): 0x%08X\n", packet_id, (unsigned)rc);

  for (int i = 0; i < 5 && az_result_succeeded(rc) && !s_puback_received; i++)
  {
    rc = az_mqtt3_client_process_loop(&client, 1000);
  }
  if (az_result_failed(rc))
  {
    printf("ERROR: 0x%08X\n", (unsigned)rc);
  }
  else if (!s_acknowledged)
  {
    printf("ERROR: PUBLISH not acknowledged\n");
  }

  // Ends the WebSocket with a close frame (and TLS with close_notify).
  az_result const disconnect_rc = az_mqtt3_client_disconnect(&client);
  printf("Disconnected: 0x%08X\n", (unsigned)disconnect_rc);
  return az_result_succeeded(rc) && s_acknowledged && az_result_succeeded(disconnect_rc) ? 0 : 1;
}
