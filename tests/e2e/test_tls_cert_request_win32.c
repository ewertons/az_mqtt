// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_tls_cert_request_win32.c
 * @brief Windows platform transport: a server that requests a client certificate.
 *
 * eng/ci/start-broker.sh runs `openssl s_server` on 8885 (requests a certificate) and 8886
 * (requires one). With no client certificate, Schannel reports SEC_I_INCOMPLETE_CREDENTIALS.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include <stdlib.h>
#include <string.h>

static union
{
  uint8_t bytes[256 * 1024];
  void* align_pointer;
  int64_t align_int64;
  double align_double;
} s_storage;

static az_mqtt_transport* const s_transport = (az_mqtt_transport*)&s_storage;

static az_result connect_tls(uint16_t port)
{
  az_mqtt_tls_options tls = az_mqtt_tls_options_default();
  tls.ca_cert_path = AZ_SPAN_FROM_STR(E2E_CA_CERT_PATH);
  az_result const rc
      = az_mqtt_transport_connect(s_transport, AZ_SPAN_FROM_STR("localhost"), port, &tls);
  az_mqtt_transport_close(s_transport);
  return rc;
}

/** @brief The handshake completes without a certificate when the server only asks for one. */
static void a_requested_certificate_is_optional(void** state)
{
  (void)state;
  assert_int_equal(connect_tls(8885), AZ_OK);
}

/** @brief A server that requires a certificate refuses the handshake. */
static void a_required_certificate_fails_the_handshake(void** state)
{
  (void)state;
  assert_int_equal(connect_tls(8886), AZ_MQTT_ERROR_TLS_HANDSHAKE);
}

static int setup(void** state)
{
  (void)state;
  return az_mqtt_transport_init(s_transport) == AZ_OK ? 0 : -1;
}

int main(void)
{
  char const* run_tls = getenv("AZ_MQTT_RUN_TLS_E2E");
  if (run_tls == NULL || strcmp(run_tls, "1") != 0)
  {
    return 0; // Same opt-in as test_ssl_win32.
  }
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_requested_certificate_is_optional),
    cmocka_unit_test(a_required_certificate_fails_the_handshake),
  };
  return cmocka_run_group_tests(tests, setup, NULL);
}
