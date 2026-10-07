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
#define RESPONSE_TOPIC_PREFIX "az-mqtt-sample/mqtt5/response/"
#define CORRELATION_DATA "request-1"

static uint8_t s_send_buffer[AZ_MQTT_SAMPLE_BUFFER_SIZE];
static uint8_t s_receive_buffer[AZ_MQTT_SAMPLE_BUFFER_SIZE];
static az_mqtt_sample_transport_storage s_transport_storage;
static az_mqtt_inflight_entry s_inflight[8];
// Of a received packet: its user properties; its subscription identifiers or reason codes.
static az_mqtt5_user_property s_decode_user_properties[8];
static int32_t s_decode_codes[8];

// RESPONSE_TOPIC_PREFIX + client id: only this client's responses arrive there.
static char s_response_topic_buffer[128];
static az_span s_response_topic;

static bool s_subscribed;
static bool s_request_sent;
static bool s_response_received;
static uint16_t s_request_packet_id;
static uint16_t s_response_packet_id;
static bool s_request_acknowledged;
static bool s_response_acknowledged;

static void on_suback(az_mqtt5_client* client, az_mqtt5_suback_data const* suback)
{
  (void)client;
  printf("[SUBACK] packet_id=%u\n", suback->packet_id);
  s_subscribed = suback->reason_code_count == 2;
  for (int32_t i = 0; i < suback->reason_code_count; i++)
  {
    // Reason codes 0x80 and above refuse the subscription.
    s_subscribed = s_subscribed && suback->reason_codes[i] < AZ_MQTT5_REASON_UNSPECIFIED_ERROR;
  }
}

static void on_puback(az_mqtt5_client* client, az_mqtt5_ack_data const* ack)
{
  (void)client;
  printf("[PUBACK] packet_id=%u reason=0x%02X\n", ack->packet_id, (unsigned)ack->reason_code);
  bool const ok
      = az_result_succeeded(ack->status) && ack->reason_code < AZ_MQTT5_REASON_UNSPECIFIED_ERROR;
  if (ack->packet_id == s_request_packet_id)
  {
    s_request_acknowledged = ok;
  }
  else if (ack->packet_id == s_response_packet_id)
  {
    s_response_acknowledged = ok;
  }
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
    uint16_t packet_id = 0;
    az_result const rc = az_mqtt5_client_publish(client, &response, &packet_id);
    printf("Response sent: 0x%08X\n", (unsigned)rc);
    // Others may send requests too: only the response to this sample's request is tracked.
    if (az_result_succeeded(rc) && s_request_sent && s_response_packet_id == 0
        && az_span_is_content_equal(publish->response_topic, s_response_topic)
        && az_span_is_content_equal(publish->correlation_data, AZ_SPAN_FROM_STR(CORRELATION_DATA)))
    {
      s_response_packet_id = packet_id;
    }
  }
  else if (s_request_sent && az_span_is_content_equal(publish->topic, s_response_topic))
  {
    // Requester: the Correlation Data tells which request this answers.
    s_response_received = s_response_received
        || az_span_is_content_equal(publish->correlation_data, AZ_SPAN_FROM_STR(CORRELATION_DATA));
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
  options.decode_user_properties = SPAN_FROM_ARRAY(s_decode_user_properties);
  options.decode_codes = SPAN_FROM_ARRAY(s_decode_codes);
  options.on_suback = on_suback;
  options.on_puback = on_puback;
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

  int const size = snprintf(
      s_response_topic_buffer,
      sizeof(s_response_topic_buffer),
      RESPONSE_TOPIC_PREFIX "%.*s",
      az_span_size(settings.client_id),
      (char const*)az_span_ptr(settings.client_id));
  if (size < 0 || size >= (int)sizeof(s_response_topic_buffer))
  {
    printf("ERROR: client id too long\n");
    return 1;
  }
  s_response_topic = az_span_create((uint8_t*)s_response_topic_buffer, size);

  // The responder listens for requests, the requester for responses.
  az_mqtt5_subscription subscriptions[2];
  memset(subscriptions, 0, sizeof(subscriptions));
  subscriptions[0].topic_filter = AZ_SPAN_FROM_STR(REQUEST_TOPIC);
  subscriptions[0].qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  subscriptions[1].topic_filter = s_response_topic;
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
    request.response_topic = s_response_topic;
    request.correlation_data = AZ_SPAN_FROM_STR(CORRELATION_DATA);
    request.user_properties = properties;
    request.user_property_count = 1;
    request.payload = AZ_SPAN_FROM_STR("{}");
    request.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
    rc = az_mqtt5_client_publish(&client, &request, &s_request_packet_id);
    printf("Request sent: 0x%08X\n", (unsigned)rc);
    s_request_sent = az_result_succeeded(rc);
  }

  // Each process_loop() returns once it has handled what arrived: PUBACKs, request, response.
  bool done = false;
  for (int i = 0; i < 10 && az_result_succeeded(rc) && s_request_sent && !done; i++)
  {
    rc = az_mqtt5_client_process_loop(&client, 1000);
    done = s_response_received && s_request_acknowledged && s_response_acknowledged;
  }
  if (az_result_failed(rc))
  {
    printf("ERROR: 0x%08X\n", (unsigned)rc);
  }
  else if (!s_subscribed)
  {
    printf("ERROR: not subscribed\n");
  }
  else if (!done)
  {
    printf(
        "ERROR: response=%d request_puback=%d response_puback=%d\n",
        s_response_received,
        s_request_acknowledged,
        s_response_acknowledged);
  }
  else
  {
    printf("Response received\n");
  }

  az_result const disconnect_rc
      = az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION);
  bool const ok = az_result_succeeded(rc) && done;
  return ok && az_result_succeeded(disconnect_rc) ? 0 : 1;
}
