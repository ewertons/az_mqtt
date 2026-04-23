// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_ssl_posix.c
 * @brief POSIX TLS integration test for az_mqtt3_client.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <az_mqtt3/az_mqtt3_client.h>
#include <azure/core/az_span.h>

#include "test_common.h"

#include <string.h>

#ifndef E2E_CA_CERT_PATH
#error "E2E_CA_CERT_PATH must be defined by CMake for TLS tests"
#endif

static az_mqtt3_e2e_fixture s_fixture;

static bool s_connack_received;
static az_mqtt3_reason_code s_connack_reason;

static void on_connack(az_mqtt3_client* client, az_mqtt3_connack_data const* connack)
{
  (void)client;
  s_connack_received = true;
  s_connack_reason = connack->reason_code;
}

static void reset_state(void)
{
  s_connack_received = false;
  s_connack_reason = AZ_MQTT3_REASON_UNSPECIFIED_ERROR;
}

static az_result init_tls_client(az_mqtt3_client* client, az_span client_id)
{
  az_mqtt3_e2e_fixture_reset(&s_fixture);

  static az_mqtt3_tls_options tls_opts;
  tls_opts = az_mqtt3_tls_options_default();
  tls_opts.ca_cert_path = AZ_SPAN_FROM_STR(E2E_CA_CERT_PATH);

  az_mqtt3_e2e_client_params params;
  memset(&params, 0, sizeof(params));
  params.client_id = client_id;
  params.hostname = AZ_SPAN_FROM_STR("localhost");
  params.port = 8883;
  params.keep_alive_seconds = 30;
  params.clean_start = true;
  params.tls_options = &tls_opts;
  params.on_connack = on_connack;

  return az_mqtt3_e2e_init_client(&s_fixture, client, &params);
}

static void test_tls_connect_and_disconnect_posix(void** state)
{
  (void)state;
  reset_state();

  az_mqtt3_client client;
  az_result rc = init_tls_client(&client, AZ_SPAN_FROM_STR("test-tls-posix-01"));
  assert_int_equal(rc, AZ_OK);

  rc = az_mqtt3_client_connect(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, AZ_MQTT3_REASON_SUCCESS);

  rc = az_mqtt3_client_disconnect(&client, AZ_MQTT3_REASON_NORMAL_DISCONNECTION);
  assert_int_equal(rc, AZ_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_tls_connect_and_disconnect_posix),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
