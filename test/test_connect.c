// Copyright (c) mqtt5_client contributors. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file test_connect.c
 * @brief Integration test: connect to a Mosquitto MQTT 5 broker and disconnect.
 *
 * Requires a running broker on localhost:1883 (plain TCP).
 * Launch with: test/start_broker.ps1 (Windows) or test/start_broker.sh (Linux)
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

// cmocka must be included after the standard headers above
#include <cmocka.h>

#include <mqtt5_client/mqtt5_client.h>
#include <azure/core/az_span.h>

#include <string.h>

// ──────────────────────── Static buffers (zero allocation) ───

#define SEND_BUF_SIZE 2048
#define RECV_BUF_SIZE 2048
#define MAX_USER_PROPS 4
#define MAX_REASON_CODES 4

static uint8_t s_send_buf[SEND_BUF_SIZE];
static uint8_t s_recv_buf[RECV_BUF_SIZE];
static uint8_t s_transport_buf[256];

static mqtt5_user_property s_connack_props[MAX_USER_PROPS];
static mqtt5_user_property s_pub_props[MAX_USER_PROPS];
static mqtt5_user_property s_sub_props[MAX_USER_PROPS];
static mqtt5_user_property s_ack_props[MAX_USER_PROPS];
static mqtt5_user_property s_disc_props[MAX_USER_PROPS];
static mqtt5_reason_code s_sub_reasons[MAX_REASON_CODES];
static int32_t s_pub_sub_ids[MAX_USER_PROPS];

// ──────────────────────── Callback tracking ──────────────────

static bool s_connack_received;
static mqtt5_reason_code s_connack_reason;
static bool s_connack_session_present;

static void on_connack(mqtt5_client* client, mqtt5_connack_data const* connack)
{
  (void)client;
  s_connack_received = true;
  s_connack_reason = connack->reason_code;
  s_connack_session_present = connack->session_present;
}

// ──────────────────────── Helper: init a client ──────────────

static az_result init_client(
    mqtt5_client* client,
    az_span client_id,
    mqtt5_on_connack_fn connack_cb)
{
  mqtt5_transport* transport = (mqtt5_transport*)s_transport_buf;
  az_result rc = mqtt5_transport_init(transport);
  if (az_result_failed(rc))
    return rc;

  mqtt5_connect_options connect_opts = mqtt5_connect_options_default();
  connect_opts.client_id = client_id;
  connect_opts.keep_alive_seconds = 30;
  connect_opts.clean_start = true;

  mqtt5_client_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.transport = transport;
  opts.send_buffer = AZ_SPAN_FROM_BUFFER(s_send_buf);
  opts.receive_buffer = AZ_SPAN_FROM_BUFFER(s_recv_buf);
  opts.connect_options = connect_opts;
  opts.hostname = AZ_SPAN_FROM_STR("localhost");
  opts.port = 1883;
  opts.tls_options = NULL;

  opts.on_connack = connack_cb;

  opts.connack_user_properties = s_connack_props;
  opts.connack_user_property_capacity = MAX_USER_PROPS;
  opts.publish_user_properties = s_pub_props;
  opts.publish_user_property_capacity = MAX_USER_PROPS;
  opts.publish_subscription_identifiers = s_pub_sub_ids;
  opts.publish_subscription_identifier_capacity = MAX_USER_PROPS;
  opts.suback_reason_codes = s_sub_reasons;
  opts.suback_reason_code_capacity = MAX_REASON_CODES;
  opts.suback_user_properties = s_sub_props;
  opts.suback_user_property_capacity = MAX_USER_PROPS;
  opts.ack_user_properties = s_ack_props;
  opts.ack_user_property_capacity = MAX_USER_PROPS;
  opts.disconnect_user_properties = s_disc_props;
  opts.disconnect_user_property_capacity = MAX_USER_PROPS;

  return mqtt5_client_init(client, &opts);
}

// ──────────────────────── Tests ──────────────────────────────

/**
 * @brief Test: connect to broker with clean start, verify CONNACK, then disconnect.
 */
static void test_connect_and_disconnect(void** state)
{
  (void)state;

  s_connack_received = false;
  s_connack_reason = MQTT5_REASON_UNSPECIFIED_ERROR;

  mqtt5_client client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-connect-01"), on_connack);
  assert_int_equal(rc, AZ_OK);

  // Connect (5 second timeout)
  rc = mqtt5_client_connect(&client, 5000);
  assert_int_equal(rc, AZ_OK);

  // Verify CONNACK
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, MQTT5_REASON_SUCCESS);
  assert_int_equal(mqtt5_client_get_state(&client), MQTT5_CLIENT_STATE_CONNECTED);

  // Clean start = true, so no previous session
  assert_false(s_connack_session_present);

  // Disconnect
  rc = mqtt5_client_disconnect(&client, MQTT5_REASON_NORMAL_DISCONNECTION);
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(mqtt5_client_get_state(&client), MQTT5_CLIENT_STATE_DISCONNECTED);
}

/**
 * @brief Test: connect, disconnect, reconnect with clean_start=false to verify session.
 */
static void test_reconnect_with_session(void** state)
{
  (void)state;

  // First connection: clean start
  s_connack_received = false;
  mqtt5_client client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-session-01"), on_connack);
  assert_int_equal(rc, AZ_OK);

  // Override session expiry to keep the session
  client.options.connect_options.session_expiry_interval = 300;

  rc = mqtt5_client_connect(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, MQTT5_REASON_SUCCESS);
  assert_false(s_connack_session_present);

  rc = mqtt5_client_disconnect(&client, MQTT5_REASON_NORMAL_DISCONNECTION);
  assert_int_equal(rc, AZ_OK);

  // Second connection: resume session
  s_connack_received = false;
  rc = init_client(&client, AZ_SPAN_FROM_STR("test-session-01"), on_connack);
  assert_int_equal(rc, AZ_OK);

  client.options.connect_options.clean_start = false;
  client.options.connect_options.session_expiry_interval = 300;

  rc = mqtt5_client_connect(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, MQTT5_REASON_SUCCESS);
  assert_true(s_connack_session_present);

  rc = mqtt5_client_disconnect(&client, MQTT5_REASON_NORMAL_DISCONNECTION);
  assert_int_equal(rc, AZ_OK);
}

/**
 * @brief Test: connecting to a non-existent host should fail with a transport error.
 */
static void test_connect_failure(void** state)
{
  (void)state;

  mqtt5_transport* transport = (mqtt5_transport*)s_transport_buf;
  az_result rc = mqtt5_transport_init(transport);
  assert_int_equal(rc, AZ_OK);

  mqtt5_connect_options connect_opts = mqtt5_connect_options_default();
  connect_opts.client_id = AZ_SPAN_FROM_STR("test-fail-01");

  mqtt5_client_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.transport = transport;
  opts.send_buffer = AZ_SPAN_FROM_BUFFER(s_send_buf);
  opts.receive_buffer = AZ_SPAN_FROM_BUFFER(s_recv_buf);
  opts.connect_options = connect_opts;
  opts.hostname = AZ_SPAN_FROM_STR("127.0.0.1");
  opts.port = 19999; // Nothing is listening here
  opts.tls_options = NULL;

  mqtt5_client client;
  rc = mqtt5_client_init(&client, &opts);
  assert_int_equal(rc, AZ_OK);

  rc = mqtt5_client_connect(&client, 3000);
  assert_true(az_result_failed(rc));
  assert_int_equal(mqtt5_client_get_state(&client), MQTT5_CLIENT_STATE_DISCONNECTED);
}

/**
 * @brief Test: operations on a disconnected client should return NOT_CONNECTED.
 */
static void test_publish_while_disconnected(void** state)
{
  (void)state;

  mqtt5_client client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-discon-01"), NULL);
  assert_int_equal(rc, AZ_OK);

  mqtt5_publish_options pub = mqtt5_publish_options_default();
  pub.topic = AZ_SPAN_FROM_STR("test/topic");
  pub.payload = AZ_SPAN_FROM_STR("hello");

  rc = mqtt5_client_publish(&client, &pub, NULL);
  assert_int_equal(rc, MQTT5_ERROR_NOT_CONNECTED);
}

// ──────────────────────── Main ───────────────────────────────

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_connect_and_disconnect),
    cmocka_unit_test(test_reconnect_with_session),
    cmocka_unit_test(test_connect_failure),
    cmocka_unit_test(test_publish_while_disconnected),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
