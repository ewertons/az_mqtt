// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_tls_mbedtls_verify.c
 * @brief mbedTLS: a verify callback installed through az_mqtt_tls_options.configure (as ESP-IDF's
 * esp_crt_bundle_attach() does) decides server verification, and its rejection is reported as
 * AZ_MQTT_ERROR_TLS_VERIFY.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <azure/core/az_span.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>

#include "test_common.h"

#include <string.h>

#ifndef E2E_CA_CERT_PATH
#error "E2E_CA_CERT_PATH must be defined by CMake for TLS tests"
#endif

static az_mqtt_e2e_fixture s_fixture;
static int s_verify_calls;

/** @brief Rejects every certificate, as the certificate bundle does for an unknown root. */
static int reject_all(void* context, mbedtls_x509_crt* crt, int depth, uint32_t* flags)
{
  (void)context;
  (void)crt;
  (void)depth;
  (void)flags;
  s_verify_calls++;
  return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
}

/** @brief Accepts what mbedTLS verified (flags unchanged). */
static int pass_through(void* context, mbedtls_x509_crt* crt, int depth, uint32_t* flags)
{
  (void)context;
  (void)crt;
  (void)depth;
  (void)flags;
  s_verify_calls++;
  return 0;
}

typedef struct
{
  int (*verify)(void*, mbedtls_x509_crt*, int, uint32_t*);
} verify_hook;

static az_result install_verify(void* native_config, void* context)
{
  mbedtls_ssl_conf_verify(
      (mbedtls_ssl_config*)native_config, ((verify_hook const*)context)->verify, NULL);
  return AZ_OK;
}

static az_result connect_with(int (*verify)(void*, mbedtls_x509_crt*, int, uint32_t*), char const* id)
{
  static verify_hook hook;
  hook.verify = verify;
  az_mqtt_e2e_fixture_reset(&s_fixture);
  s_verify_calls = 0;

  static az_mqtt_tls_options tls_opts;
  tls_opts = az_mqtt_tls_options_default();
  tls_opts.ca_cert_path = AZ_SPAN_FROM_STR(E2E_CA_CERT_PATH);
  tls_opts.configure = install_verify;
  tls_opts.configure_context = &hook;

  az_mqtt_e2e_client_params params;
  memset(&params, 0, sizeof(params));
  params.client_id = az_span_create_from_str((char*)(uintptr_t)id);
  params.hostname = AZ_SPAN_FROM_STR("localhost");
  params.port = 8883;
  params.keep_alive_seconds = 30;
  params.clean_start = true;
  params.tls_options = &tls_opts;

  static AZ_MQTT_T(client) client;
  az_result rc = az_mqtt_e2e_init_client(&s_fixture, &client, &params);
  if (az_result_failed(rc))
  {
    return rc;
  }
  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  if (az_result_succeeded(rc))
  {
    rc = AZ_MQTT_TEST_DISCONNECT(&client);
  }
  return rc;
}

static void test_verify_callback_accepts(void** state)
{
  (void)state;
  assert_int_equal(connect_with(pass_through, "test-tls-vrfy-01"), AZ_OK);
  assert_true(s_verify_calls > 0);
}

static void test_verify_callback_rejection_is_tls_verify(void** state)
{
  (void)state;
  assert_int_equal(connect_with(reject_all, "test-tls-vrfy-02"), AZ_MQTT_ERROR_TLS_VERIFY);
  assert_true(s_verify_calls > 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_verify_callback_accepts),
    cmocka_unit_test(test_verify_callback_rejection_is_tls_verify),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
