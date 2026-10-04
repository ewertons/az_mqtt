// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt5_sample_request_response.c
 * @brief Sample: MQTT 5.0 request/response with PUBLISH properties. One client plays both roles:
 * - requester: publishes a request with a Response Topic, Correlation Data and a User Property;
 * - responder: answers each request on its Response Topic, echoing its Correlation Data.
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

#define REQUEST_TOPIC "az-mqtt-sample/mqtt5/request"
#define RESPONSE_TOPIC "az-mqtt-sample/mqtt5/response"

static uint8_t s_send_buffer[AZ_MQTT_SAMPLE_BUFFER_SIZE];
static uint8_t s_receive_buffer[AZ_MQTT_SAMPLE_BUFFER_SIZE];
static az_mqtt_sample_transport_storage s_transport_storage;
static az_mqtt_inflight_entry s_inflight[8];
static az_mqtt5_user_property s_publish_user_properties[8];
static az_mqtt5_reason_code s_suback_reason_codes[8];

static bool s_subscribed;
static bool s_response_received;

static void on_suback(az_mqtt5_client* client, az_mqtt5_suback_data const* suback)
{
  (void)client;
  printf("[SUBACK] packet_id=%u\n", suback->packet_id);
  s_subscribed = true;
}

static void on_publish(az_mqtt5_client* client, az_mqtt5_publish_data const* publish)
{
  printf(
      "[PUBLISH received] topic=\"%.*s\" payload=\"%.*s\" correlation=\"%.*s\"\n",
      az_span_size(publish->topic),
      (char const*)az_span_ptr(publish->topic),
      az_span_size(publish->payload),
      (char const*)az_span_ptr(publish->payload),
      az_span_size(publish->correlation_data),
      (char const*)az_span_ptr(publish->correlation_data));
  for (int32_t i = 0; i < publish->user_property_count; i++)
  {
    printf(
        "  user property %.*s=%.*s\n",
        az_span_size(publish->user_properties[i].key),
        (char const*)az_span_ptr(publish->user_properties[i].key),
        az_span_size(publish->user_properties[i].value),
        (char const*)az_span_ptr(publish->user_properties[i].value));
  }

  if (az_span_is_content_equal(publish->topic, AZ_SPAN_FROM_STR(REQUEST_TOPIC))
      && az_span_size(publish->response_topic) > 0)
  {
    // Responder: answer on the requester's Response Topic, with its Correlation Data. The
    // received spans are valid during this callback, and publish() encodes them right away.
    az_mqtt5_publish_options response = az_mqtt5_publish_options_default();
    response.topic = publish->response_topic;
    response.correlation_data = publish->correlation_data;
    response.payload = AZ_SPAN_FROM_STR("{\"status\":\"ok\"}");
    response.content_type = AZ_SPAN_FROM_STR("application/json");
    response.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
    az_result const rc = az_mqtt5_client_publish(client, &response, NULL);
    printf("Response sent: 0x%08X\n", (unsigned)rc);
  }
  else if (az_span_is_content_equal(publish->topic, AZ_SPAN_FROM_STR(RESPONSE_TOPIC)))
  {
    // Requester: the Correlation Data tells which request this answers.
    s_response_received
        = az_span_is_content_equal(publish->correlation_data, AZ_SPAN_FROM_STR("request-1"));
  }
}

int main(void)
{
  az_mqtt_sample_settings settings;
  if (!az_mqtt_sample_settings_read(&settings, 1883, "az-mqtt5-sample-request-response"))
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
  // Received User Properties are decoded into this caller storage (extras are dropped).
  options.buffers.publish_user_properties = SPAN_FROM_ARRAY(s_publish_user_properties);
  options.buffers.suback_reason_codes = SPAN_FROM_ARRAY(s_suback_reason_codes);
  options.on_suback = on_suback;
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

  // The responder listens for requests, the requester for responses.
  az_mqtt5_subscription subscriptions[2];
  memset(subscriptions, 0, sizeof(subscriptions));
  subscriptions[0].topic_filter = AZ_SPAN_FROM_STR(REQUEST_TOPIC);
  subscriptions[0].qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  subscriptions[1].topic_filter = AZ_SPAN_FROM_STR(RESPONSE_TOPIC);
  subscriptions[1].qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  rc = az_mqtt5_client_subscribe(&client, subscriptions, 2, NULL);
  for (int i = 0; i < 5 && az_result_succeeded(rc) && !s_subscribed; i++)
  {
    rc = az_mqtt5_client_process_loop(&client, 1000);
  }

  if (az_result_succeeded(rc) && s_subscribed)
  {
    az_mqtt5_user_property properties[1];
    properties[0].key = AZ_SPAN_FROM_STR("operation");
    properties[0].value = AZ_SPAN_FROM_STR("get-status");

    az_mqtt5_publish_options request = az_mqtt5_publish_options_default();
    request.topic = AZ_SPAN_FROM_STR(REQUEST_TOPIC);
    request.response_topic = AZ_SPAN_FROM_STR(RESPONSE_TOPIC);
    request.correlation_data = AZ_SPAN_FROM_STR("request-1");
    request.user_properties = properties;
    request.user_property_count = 1;
    request.payload = AZ_SPAN_FROM_STR("{}");
    request.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
    rc = az_mqtt5_client_publish(&client, &request, NULL);
    printf("Request sent: 0x%08X\n", (unsigned)rc);
  }

  for (int i = 0; i < 5 && az_result_succeeded(rc) && !s_response_received; i++)
  {
    rc = az_mqtt5_client_process_loop(&client, 1000);
  }
  printf(s_response_received ? "Response received\n" : "ERROR: no response\n");

  az_result const disconnect_rc
      = az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION);
  return s_response_received && az_result_succeeded(disconnect_rc) ? 0 : 1;
}
