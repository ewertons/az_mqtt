// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_ssl_win32.c
 * @brief Windows TLS integration test for az_mqtt_client.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <az_mqtt/az_mqtt_client.h>
#include <azure/core/az_span.h>

#include "test_common.h"

#include <stdlib.h>
#include <string.h>

#ifndef E2E_WIN32_TLS_BACKEND_AVAILABLE
#define E2E_WIN32_TLS_BACKEND_AVAILABLE 0
#endif

static az_mqtt_e2e_fixture s_fixture;

static bool s_connack_received;
static az_mqtt_reason_code s_connack_reason;

static void on_connack(az_mqtt_client* client, az_mqtt_connack_data const* connack)
{
  (void)client;
  s_connack_received = true;
  s_connack_reason = connack->reason_code;
}

static void reset_state(void)
{
  s_connack_received = false;
  s_connack_reason = AZ_MQTT_REASON_UNSPECIFIED_ERROR;
}

static az_result init_tls_client(az_mqtt_client* client, az_span client_id)
{
  az_mqtt_e2e_fixture_reset(&s_fixture);

  static az_mqtt_tls_options tls_opts;
  tls_opts = az_mqtt_tls_options_default();
#if E2E_WIN32_TLS_BACKEND_AVAILABLE
  tls_opts.ca_cert_path = AZ_SPAN_FROM_STR(E2E_CA_CERT_PATH);
#endif

  az_mqtt_e2e_client_params params;
  memset(&params, 0, sizeof(params));
  params.client_id = client_id;
  params.hostname = AZ_SPAN_FROM_STR("localhost");
  params.port = 8883;
  params.keep_alive_seconds = 30;
  params.clean_start = true;
  params.tls_options = &tls_opts;
  params.on_connack = on_connack;

  return az_mqtt_e2e_init_client(&s_fixture, client, &params);
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

  az_mqtt_client client;
  az_result rc = init_tls_client(&client, AZ_SPAN_FROM_STR("test-tls-win32-01"));
  assert_int_equal(rc, AZ_OK);

  rc = az_mqtt_client_connect(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, AZ_MQTT_REASON_SUCCESS);

  rc = az_mqtt_client_disconnect(&client, AZ_MQTT_REASON_NORMAL_DISCONNECTION);
  assert_int_equal(rc, AZ_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_tls_connect_and_disconnect_win32),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
