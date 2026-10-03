// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_close_notify_win32.c
 * @brief Windows platform transport: TLS close_notify, observed through the e2e proxy.
 *
 * The proxy sees TLS record types: an alert (21) is visible since Schannel with SCHANNEL_CRED
 * negotiates at most TLS 1.2.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include "e2e_proxy.h"

#include <stdlib.h>
#include <string.h>

#include <windows.h>

static union
{
  uint8_t bytes[256 * 1024];
  void* align_pointer;
  int64_t align_int64;
  double align_double;
} s_storage;
static az_mqtt_transport* const s_transport = (az_mqtt_transport*)&s_storage;
static e2e_proxy* s_proxy;
static az_mqtt_proxy_options s_proxy_options;
static az_mqtt_tls_options s_tls;
static int s_socket_errors;
static int s_other_errors;

static void on_error(az_mqtt_native_error const* error, void* context)
{
  (void)context;
  if (error->source == AZ_MQTT_NATIVE_ERROR_SOCKET)
  {
    s_socket_errors++;
  }
  else
  {
    s_other_errors++;
  }
}

/** @brief Wait up to 2 s for the proxy to have seen @p n client alerts. */
static int wait_alerts(int n)
{
  for (int i = 0; i < 100 && e2e_proxy_client_alerts(s_proxy) < n; i++)
  {
    Sleep(20);
  }
  Sleep(100); // Any extra alert.
  return e2e_proxy_client_alerts(s_proxy);
}

static int setup(void** state)
{
  (void)state;
  e2e_proxy_options options;
  memset(&options, 0, sizeof(options));
  s_proxy = e2e_proxy_start(&options);
  if (s_proxy == NULL || az_mqtt_transport_init(s_transport) != AZ_OK)
  {
    return -1;
  }
  memset(&s_proxy_options, 0, sizeof(s_proxy_options));
  s_proxy_options.host = AZ_SPAN_FROM_STR("127.0.0.1");
  s_proxy_options.port = e2e_proxy_port(s_proxy);
  s_tls = az_mqtt_tls_options_default();
  s_tls.ca_cert_path = AZ_SPAN_FROM_STR(E2E_CA_CERT_PATH);
  az_mqtt_transport_set_error_callback(s_transport, on_error, NULL);
  return az_mqtt_transport_set_proxy(s_transport, &s_proxy_options) == AZ_OK ? 0 : -1;
}

static int teardown(void** state)
{
  (void)state;
  az_mqtt_transport_close(s_transport);
  e2e_proxy_stop(s_proxy);
  return 0;
}

static void connect_tls(void)
{
  s_socket_errors = 0;
  s_other_errors = 0;
  assert_int_equal(
      az_mqtt_transport_connect(s_transport, AZ_SPAN_FROM_STR("localhost"), 8883, &s_tls), AZ_OK);
  assert_int_equal(wait_alerts(0), 0);
}

static void close_sends_close_notify(void** state)
{
  (void)state;
  connect_tls();
  az_mqtt_transport_close(s_transport);
  assert_int_equal(wait_alerts(1), 1);
  assert_int_equal(s_socket_errors + s_other_errors, 0);
}

static void shutdown_then_close_sends_it_once(void** state)
{
  (void)state;
  connect_tls();
  az_mqtt_transport_shutdown(s_transport);
  assert_int_equal(wait_alerts(1), 1);
  az_mqtt_transport_close(s_transport);
  assert_int_equal(wait_alerts(2), 1);
  assert_int_equal(s_socket_errors + s_other_errors, 0);
}

static void a_failed_send_of_it_is_reported(void** state)
{
  (void)state;
  connect_tls();
  assert_true(e2e_proxy_reset_client(s_proxy));
  Sleep(200); // The reset reaches the client.
  az_mqtt_transport_close(s_transport);
  assert_int_equal(s_socket_errors, 1);
  assert_int_equal(s_other_errors, 0);
}

int main(void)
{
  char const* run_tls = getenv("AZ_MQTT_RUN_TLS_E2E");
  if (run_tls == NULL || strcmp(run_tls, "1") != 0)
  {
    return 0; // Same opt-in as test_ssl_win32.
  }
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(close_sends_close_notify),
    cmocka_unit_test(shutdown_then_close_sends_it_once),
    cmocka_unit_test(a_failed_send_of_it_is_reported),
  };
  return cmocka_run_group_tests(tests, setup, teardown);
}
