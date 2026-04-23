// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_connect.c
 * @brief Integration tests for connection and state transition behaviors.
 *
 * Requires a running broker on localhost:1883 (plain TCP).
 * Launch with: test/start_broker.ps1 (Windows) or test/start_broker.sh (Linux)
 *
 * Test matrix (API behavior -> test):
 *  - connect() success state transition -> test_connect_and_disconnect
 *  - connect() invalid state when already connected -> test_connect_when_already_connected
 *  - connect() failure on unreachable port -> test_connect_failure
 *  - reconnect with clean_start=false session resume -> test_reconnect_with_session
 *  - process_loop() while disconnected -> test_process_loop_while_disconnected
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

// cmocka must be included after the standard headers above

#define AZ_MQTT3_SPAN_FROM_ARRAY(ARRAY) \
  AZ_SPAN_FROM_BUFFER(*(uint8_t(*)[sizeof(ARRAY)])(ARRAY))
#include <cmocka.h>

#include <az_mqtt3/az_mqtt3_client.h>
#include <azure/core/az_span.h>

#include "test_common.h"

#include <string.h>

static az_mqtt3_e2e_fixture s_fixture;

// ──────────────────────── Callback tracking ──────────────────

static bool s_connack_received;
static az_mqtt3_reason_code s_connack_reason;
static bool s_connack_session_present;

static void reset_callback_state(void)
{
  s_connack_received = false;
  s_connack_reason = AZ_MQTT3_REASON_UNSPECIFIED_ERROR;
  s_connack_session_present = false;
}

static void on_connack(az_mqtt3_client* client, az_mqtt3_connack_data const* connack)
{
  (void)client;
  s_connack_received = true;
  s_connack_reason = connack->reason_code;
  s_connack_session_present = connack->session_present;
}

// ──────────────────────── Helper: init a client ──────────────

static az_result init_client(az_mqtt3_client* client, az_span client_id)
{
  az_mqtt3_e2e_fixture_reset(&s_fixture);

  az_mqtt3_e2e_client_params params;
  memset(&params, 0, sizeof(params));
  params.client_id = client_id;
  params.hostname = AZ_SPAN_FROM_STR("localhost");
  params.port = 1883;
  params.keep_alive_seconds = 30;
  params.clean_start = true;
  params.tls_options = NULL;
  params.on_connack = on_connack;

  return az_mqtt3_e2e_init_client(&s_fixture, client, &params);
}

// ──────────────────────── Tests ──────────────────────────────

/**
 * @brief Test: connect to broker with clean start, verify CONNACK, then disconnect.
 */
static void test_connect_and_disconnect(void** state)
{
  (void)state;
  reset_callback_state();

  az_mqtt3_client client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-connect-01"));
  assert_int_equal(rc, AZ_OK);

  // Connect (5 second timeout)
  rc = az_mqtt3_client_connect(&client, 5000);
  assert_int_equal(rc, AZ_OK);

  // Verify CONNACK
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, AZ_MQTT3_REASON_SUCCESS);
  assert_int_equal(az_mqtt3_client_get_state(&client), AZ_MQTT3_CLIENT_STATE_CONNECTED);

  // Clean start = true, so no previous session
  assert_false(s_connack_session_present);

  // Disconnect
  rc = az_mqtt3_client_disconnect(&client, AZ_MQTT3_REASON_NORMAL_DISCONNECTION);
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(az_mqtt3_client_get_state(&client), AZ_MQTT3_CLIENT_STATE_DISCONNECTED);

  // Disconnecting an already disconnected client is idempotent.
  rc = az_mqtt3_client_disconnect(&client, AZ_MQTT3_REASON_NORMAL_DISCONNECTION);
  assert_int_equal(rc, AZ_OK);
}

/**
 * @brief Test: calling connect while connected returns INVALID_STATE.
 */
static void test_connect_when_already_connected(void** state)
{
  (void)state;
  reset_callback_state();

  az_mqtt3_client client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-connect-dup-01"));
  assert_int_equal(rc, AZ_OK);

  rc = az_mqtt3_client_connect(&client, 5000);
  assert_int_equal(rc, AZ_OK);

  rc = az_mqtt3_client_connect(&client, 5000);
  assert_int_equal(rc, AZ_MQTT3_ERROR_INVALID_STATE);

  rc = az_mqtt3_client_disconnect(&client, AZ_MQTT3_REASON_NORMAL_DISCONNECTION);
  assert_int_equal(rc, AZ_OK);
}

/**
 * @brief Test: connect, disconnect, reconnect with clean_start=false to verify session.
 */
static void test_reconnect_with_session(void** state)
{
  (void)state;
  reset_callback_state();
  az_span client_id = AZ_SPAN_FROM_STR("test-session-01");

  // First connection: clean start
  az_mqtt3_client client;
  az_result rc = init_client(&client, client_id);
  assert_int_equal(rc, AZ_OK);

  // Override session expiry to keep the session
  client.options.connect_options.session_expiry_interval = 300;

  rc = az_mqtt3_client_connect(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, AZ_MQTT3_REASON_SUCCESS);
  assert_false(s_connack_session_present);

  rc = az_mqtt3_client_disconnect(&client, AZ_MQTT3_REASON_NORMAL_DISCONNECTION);
  assert_int_equal(rc, AZ_OK);

  // Second connection: resume session
  s_connack_received = false;
  rc = init_client(&client, client_id);
  assert_int_equal(rc, AZ_OK);

  client.options.connect_options.clean_start = false;
  client.options.connect_options.session_expiry_interval = 300;

  rc = az_mqtt3_client_connect(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, AZ_MQTT3_REASON_SUCCESS);
  assert_true(s_connack_session_present);

  rc = az_mqtt3_client_disconnect(&client, AZ_MQTT3_REASON_NORMAL_DISCONNECTION);
  assert_int_equal(rc, AZ_OK);
}

/**
 * @brief Test: connecting to a non-existent host should fail with a transport error.
 */
static void test_connect_failure(void** state)
{
  (void)state;
  reset_callback_state();

  az_mqtt3_e2e_fixture_reset(&s_fixture);

  az_mqtt3_transport* transport = (az_mqtt3_transport*)s_fixture.transport_buf;
  az_result rc = az_mqtt3_transport_init(transport);
  assert_int_equal(rc, AZ_OK);

  az_mqtt3_connect_options connect_opts = az_mqtt3_connect_options_default();
  connect_opts.client_id = AZ_SPAN_FROM_STR("test-fail-01");

  az_mqtt3_client_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.transport = transport;
  opts.send_buffer = AZ_SPAN_FROM_BUFFER(s_fixture.send_buf);
  opts.receive_buffer = AZ_SPAN_FROM_BUFFER(s_fixture.recv_buf);
  opts.connect_options = connect_opts;
  opts.hostname = AZ_SPAN_FROM_STR("127.0.0.1");
  opts.port = 19999; // Nothing is listening here
  opts.tls_options = NULL;
  opts.buffers.connack_user_properties = AZ_MQTT3_SPAN_FROM_ARRAY(s_fixture.connack_props);
  opts.buffers.publish_user_properties = AZ_MQTT3_SPAN_FROM_ARRAY(s_fixture.publish_props);
  opts.buffers.publish_subscription_identifiers = AZ_MQTT3_SPAN_FROM_ARRAY(s_fixture.publish_subscription_ids);
  opts.buffers.suback_reason_codes = AZ_MQTT3_SPAN_FROM_ARRAY(s_fixture.suback_reasons);
  opts.buffers.suback_user_properties = AZ_MQTT3_SPAN_FROM_ARRAY(s_fixture.suback_props);
  opts.buffers.ack_user_properties = AZ_MQTT3_SPAN_FROM_ARRAY(s_fixture.ack_props);
  opts.buffers.disconnect_user_properties = AZ_MQTT3_SPAN_FROM_ARRAY(s_fixture.disconnect_props);

  az_mqtt3_client client;
  rc = az_mqtt3_client_init(&client, &opts);
  assert_int_equal(rc, AZ_OK);

  rc = az_mqtt3_client_connect(&client, 3000);
  assert_true(az_result_failed(rc));
  assert_int_equal(az_mqtt3_client_get_state(&client), AZ_MQTT3_CLIENT_STATE_DISCONNECTED);
}

/**
 * @brief Test: process_loop on a disconnected client returns NOT_CONNECTED.
 */
static void test_process_loop_while_disconnected(void** state)
{
  (void)state;
  reset_callback_state();

  az_mqtt3_client client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-loop-disc-01"));
  assert_int_equal(rc, AZ_OK);

  rc = az_mqtt3_client_process_loop(&client, 10);
  assert_int_equal(rc, AZ_MQTT3_ERROR_NOT_CONNECTED);
}

// ──────────────────────── Main ───────────────────────────────

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_connect_and_disconnect),
    cmocka_unit_test(test_connect_when_already_connected),
    cmocka_unit_test(test_reconnect_with_session),
    cmocka_unit_test(test_connect_failure),
    cmocka_unit_test(test_process_loop_while_disconnected),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
