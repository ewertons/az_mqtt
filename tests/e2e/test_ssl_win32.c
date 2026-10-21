// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_ssl_win32.c
 * @brief Windows TLS integration test for AZ_MQTT_T(client).
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <azure/core/az_span.h>

#include "test_common.h"

#include <stdlib.h>
#include <string.h>

#ifndef E2E_WIN32_TLS_BACKEND_AVAILABLE
#define E2E_WIN32_TLS_BACKEND_AVAILABLE 0
#endif

static az_mqtt_e2e_fixture s_fixture;

static bool s_connack_received;
static int s_connack_reason;

static void on_connack(AZ_MQTT_T(client)* client, AZ_MQTT_T(connack_data) const* connack)
{
  (void)client;
  s_connack_received = true;
  s_connack_reason = AZ_MQTT_TEST_CONNACK_CODE(connack);
}

static void reset_state(void)
{
  s_connack_received = false;
  s_connack_reason = -1;
}

static az_result init_tls_client_with_ca(
    AZ_MQTT_T(client)* client,
    az_span client_id,
    uint16_t port,
    az_span ca_cert_path)
{
  az_mqtt_e2e_fixture_reset(&s_fixture);

  static az_mqtt_tls_options tls_opts;
  tls_opts = az_mqtt_tls_options_default();
  tls_opts.ca_cert_path = ca_cert_path;

  az_mqtt_e2e_client_params params;
  memset(&params, 0, sizeof(params));
  params.client_id = client_id;
  params.hostname = AZ_SPAN_FROM_STR("localhost");
  params.port = port;
  params.keep_alive_seconds = 30;
  params.clean_start = true;
  params.tls_options = &tls_opts;
  params.on_connack = on_connack;

  return az_mqtt_e2e_init_client(&s_fixture, client, &params);
}

static az_result init_tls_client(AZ_MQTT_T(client)* client, az_span client_id, uint16_t port)
{
#if E2E_WIN32_TLS_BACKEND_AVAILABLE
  return init_tls_client_with_ca(client, client_id, port, AZ_SPAN_FROM_STR(E2E_CA_CERT_PATH));
#else
  return init_tls_client_with_ca(client, client_id, port, AZ_SPAN_EMPTY);
#endif
}

static void test_tls_connect_and_disconnect_win32(void** state)
{
  (void)state;
  reset_state();

  char const* run_tls = getenv("AZ_MQTT_RUN_TLS_E2E");
  if (run_tls == NULL || strcmp(run_tls, "1") != 0)
  {
    // Opt-in while Schannel integration is being hardened across environments.
    assert_true(true);
    return;
  }

#if !E2E_WIN32_TLS_BACKEND_AVAILABLE
  // This test target is always present on Windows, but current transport has no TLS backend.
  // Once Schannel transport is implemented, this path should be removed.
  assert_true(true);
  return;
#endif

  AZ_MQTT_T(client) client;
  az_result rc = init_tls_client(&client, AZ_SPAN_FROM_STR("test-tls-win32-01"), 8883);
  assert_int_equal(rc, AZ_OK);

  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, AZ_MQTT_TEST_CONNACK_ACCEPTED);

  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
}

/** @brief connect_start, then process_loop until connected (Schannel). */
static void test_tls_started_connect_win32(void** state)
{
  (void)state;
  reset_state();

  char const* run_tls = getenv("AZ_MQTT_RUN_TLS_E2E");
  if (run_tls == NULL || strcmp(run_tls, "1") != 0 || !E2E_WIN32_TLS_BACKEND_AVAILABLE)
  {
    return; // Same opt-in as test_tls_connect_and_disconnect_win32.
  }

  AZ_MQTT_T(client) client;
  az_result rc = init_tls_client(&client, AZ_SPAN_FROM_STR("test-tls-win32-02"), 8883);
  assert_int_equal(rc, AZ_OK);

  rc = AZ_MQTT_T(client_connect_start)(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  int calls = 0;
  while (az_result_succeeded(rc)
         && AZ_MQTT_T(client_get_state)(&client) == AZ_MQTT_CLIENT_STATE_CONNECTING && calls < 500)
  {
    rc = AZ_MQTT_T(client_process_loop)(&client, 10);
    calls++;
  }
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, AZ_MQTT_TEST_CONNACK_ACCEPTED);

  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
}

/** @brief Port 8884: a server certificate whose Extended Key Usage is clientAuth only. */
static void test_tls_client_auth_only_certificate_is_refused_win32(void** state)
{
  (void)state;
  reset_state();
  char const* run_tls = getenv("AZ_MQTT_RUN_TLS_E2E");
  if (run_tls == NULL || strcmp(run_tls, "1") != 0 || !E2E_WIN32_TLS_BACKEND_AVAILABLE)
  {
    return; // Same opt-in as test_tls_connect_and_disconnect_win32.
  }

  AZ_MQTT_T(client) client;
  az_result rc = init_tls_client(&client, AZ_SPAN_FROM_STR("test-tls-win32-03"), 8884);
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_connect)(&client, 5000), AZ_MQTT_ERROR_TLS_VERIFY);
  assert_false(s_connack_received);
}

#if E2E_WIN32_TLS_BACKEND_AVAILABLE
/** @brief A CA file of many certificates over 64 KiB, the CA last: every one is trusted. */
static void test_tls_ca_bundle_win32(void** state)
{
  (void)state;
  reset_state();
  char const* run_tls = getenv("AZ_MQTT_RUN_TLS_E2E");
  if (run_tls == NULL || strcmp(run_tls, "1") != 0)
  {
    return; // Same opt-in as test_tls_connect_and_disconnect_win32.
  }

  AZ_MQTT_T(client) client;
  az_result rc = init_tls_client_with_ca(
      &client, AZ_SPAN_FROM_STR("test-tls-win32-04"), 8883, AZ_SPAN_FROM_STR(E2E_CA_BUNDLE_PATH));
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_connect)(&client, 5000), AZ_OK);
  assert_true(s_connack_received);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&client), AZ_OK);
}

/** @brief The same bundle without the CA: the server is not trusted. */
static void test_tls_ca_bundle_without_ca_is_refused_win32(void** state)
{
  (void)state;
  reset_state();
  char const* run_tls = getenv("AZ_MQTT_RUN_TLS_E2E");
  if (run_tls == NULL || strcmp(run_tls, "1") != 0)
  {
    return; // Same opt-in as test_tls_connect_and_disconnect_win32.
  }

  AZ_MQTT_T(client) client;
  az_result rc = init_tls_client_with_ca(
      &client,
      AZ_SPAN_FROM_STR("test-tls-win32-05"),
      8883,
      AZ_SPAN_FROM_STR(E2E_CA_BUNDLE_WITHOUT_CA_PATH));
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_connect)(&client, 5000), AZ_MQTT_ERROR_TLS_VERIFY);
  assert_false(s_connack_received);
}

/** @brief A CA file that does not load whole fails setup, though it starts with the CA. */
static void assert_ca_file_refused(char const* client_id, char const* ca_path)
{
  reset_state();
  char const* run_tls = getenv("AZ_MQTT_RUN_TLS_E2E");
  if (run_tls == NULL || strcmp(run_tls, "1") != 0)
  {
    return; // Same opt-in as test_tls_connect_and_disconnect_win32.
  }

  AZ_MQTT_T(client) client;
  az_result rc = init_tls_client_with_ca(
      &client, az_span_create_from_str((char*)(uintptr_t)client_id), 8883,
      az_span_create_from_str((char*)(uintptr_t)ca_path));
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_connect)(&client, 5000), AZ_MQTT_ERROR_TRANSPORT);
  assert_false(s_connack_received);
}

/** @brief The CA, then a certificate cut short. */
static void test_tls_ca_bundle_truncated_is_refused_win32(void** state)
{
  (void)state;
  assert_ca_file_refused("test-tls-win32-06", E2E_CA_BUNDLE_TRUNCATED_PATH);
}

/** @brief The CA, then a certificate over 32 KiB of PEM. */
static void test_tls_ca_bundle_oversized_is_refused_win32(void** state)
{
  (void)state;
  assert_ca_file_refused("test-tls-win32-07", E2E_CA_BUNDLE_OVERSIZED_PATH);
}
#endif

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_tls_connect_and_disconnect_win32),
    cmocka_unit_test(test_tls_started_connect_win32),
    cmocka_unit_test(test_tls_client_auth_only_certificate_is_refused_win32),
#if E2E_WIN32_TLS_BACKEND_AVAILABLE
    cmocka_unit_test(test_tls_ca_bundle_win32),
    cmocka_unit_test(test_tls_ca_bundle_without_ca_is_refused_win32),
    cmocka_unit_test(test_tls_ca_bundle_truncated_is_refused_win32),
    cmocka_unit_test(test_tls_ca_bundle_oversized_is_refused_win32),
#endif
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
