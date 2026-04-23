// Copyright (c) az_mqtt5_client contributors. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file test_publish.c
 * @brief Integration tests for publish flows in az_mqtt5_client.
 *
 * Requires a running broker on localhost:1883 (plain TCP).
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

// cmocka must be included after the standard headers above
#include <cmocka.h>

#include <az_mqtt5/az_mqtt5_client.h>
#include <azure/core/az_span.h>

#include "test_common.h"

#include <string.h>
#define PROCESS_LOOP_TIMEOUT_MS 100
#define WAIT_ITERATIONS 60

static az_mqtt5_e2e_fixture s_fixture;

static bool s_puback_received;
static uint16_t s_puback_packet_id;
static az_mqtt5_reason_code s_puback_reason;

static bool s_pubcomp_received;
static uint16_t s_pubcomp_packet_id;
static az_mqtt5_reason_code s_pubcomp_reason;

static void reset_callback_state(void)
{
  s_puback_received = false;
  s_puback_packet_id = 0;
  s_puback_reason = AZ_MQTT5_REASON_UNSPECIFIED_ERROR;

  s_pubcomp_received = false;
  s_pubcomp_packet_id = 0;
  s_pubcomp_reason = AZ_MQTT5_REASON_UNSPECIFIED_ERROR;
}

static bool cond_puback_received(void)
{
  return s_puback_received;
}

static bool cond_pubcomp_received(void)
{
  return s_pubcomp_received;
}

static void on_puback(az_mqtt5_client* client, az_mqtt5_ack_data const* ack)
{
  (void)client;
  s_puback_received = true;
  s_puback_packet_id = ack->packet_id;
  s_puback_reason = ack->reason_code;
}

static void on_pubcomp(az_mqtt5_client* client, az_mqtt5_ack_data const* ack)
{
  (void)client;
  s_pubcomp_received = true;
  s_pubcomp_packet_id = ack->packet_id;
  s_pubcomp_reason = ack->reason_code;
}

static az_result init_client(az_mqtt5_client* client, az_span client_id)
{
  az_mqtt5_e2e_fixture_reset(&s_fixture);

  az_mqtt5_e2e_client_params params;
  memset(&params, 0, sizeof(params));
  params.client_id = client_id;
  params.hostname = AZ_SPAN_FROM_STR("localhost");
  params.port = 1883;
  params.keep_alive_seconds = 30;
  params.clean_start = true;
  params.tls_options = NULL;
  params.on_puback = on_puback;
  params.on_pubcomp = on_pubcomp;

  return az_mqtt5_e2e_init_client(&s_fixture, client, &params);
}

static void test_publish_while_disconnected(void** state)
{
  (void)state;
  reset_callback_state();

  az_mqtt5_client client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-publish-disc"));
  assert_int_equal(rc, AZ_OK);

  az_mqtt5_publish_options pub = az_mqtt5_publish_options_default();
  pub.topic = AZ_SPAN_FROM_STR("az/e2e/publish/disconnected");
  pub.payload = AZ_SPAN_FROM_STR("hello");

  rc = az_mqtt5_client_publish(&client, &pub, NULL);
  assert_int_equal(rc, AZ_MQTT5_ERROR_NOT_CONNECTED);
}

static void test_publish_qos1_puback(void** state)
{
  (void)state;
  reset_callback_state();

  az_mqtt5_client client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-publish-qos1"));
  assert_int_equal(rc, AZ_OK);

  rc = az_mqtt5_client_connect(&client, 5000);
  assert_int_equal(rc, AZ_OK);

  az_mqtt5_publish_options pub = az_mqtt5_publish_options_default();
  pub.topic = AZ_SPAN_FROM_STR("az/e2e/publish/qos1");
  pub.payload = AZ_SPAN_FROM_STR("qos1-msg");
  pub.qos = AZ_MQTT5_QOS_AT_LEAST_ONCE;

  uint16_t packet_id = 0;
  rc = az_mqtt5_client_publish(&client, &pub, &packet_id);
  assert_int_equal(rc, AZ_OK);
  assert_true(packet_id > 0);

  rc = az_mqtt5_e2e_wait_until(&client, WAIT_ITERATIONS, PROCESS_LOOP_TIMEOUT_MS, cond_puback_received);
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(s_puback_packet_id, packet_id);
  assert_true(
      s_puback_reason == AZ_MQTT5_REASON_SUCCESS
      || s_puback_reason == AZ_MQTT5_REASON_NO_MATCHING_SUBSCRIBERS);

  rc = az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION);
  assert_int_equal(rc, AZ_OK);
}

static void test_publish_qos2_pubcomp(void** state)
{
  (void)state;
  reset_callback_state();

  az_mqtt5_client client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-publish-qos2"));
  assert_int_equal(rc, AZ_OK);

  rc = az_mqtt5_client_connect(&client, 5000);
  assert_int_equal(rc, AZ_OK);

  az_mqtt5_publish_options pub = az_mqtt5_publish_options_default();
  pub.topic = AZ_SPAN_FROM_STR("az/e2e/publish/qos2");
  pub.payload = AZ_SPAN_FROM_STR("qos2-msg");
  pub.qos = AZ_MQTT5_QOS_EXACTLY_ONCE;

  uint16_t packet_id = 0;
  rc = az_mqtt5_client_publish(&client, &pub, &packet_id);
  assert_int_equal(rc, AZ_OK);
  assert_true(packet_id > 0);

  rc = az_mqtt5_e2e_wait_until(&client, WAIT_ITERATIONS, PROCESS_LOOP_TIMEOUT_MS, cond_pubcomp_received);
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(s_pubcomp_packet_id, packet_id);
  assert_int_equal((int)s_pubcomp_reason, (int)AZ_MQTT5_REASON_SUCCESS);

  rc = az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION);
  assert_int_equal(rc, AZ_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_publish_while_disconnected),
    cmocka_unit_test(test_publish_qos1_puback),
    cmocka_unit_test(test_publish_qos2_pubcomp),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
