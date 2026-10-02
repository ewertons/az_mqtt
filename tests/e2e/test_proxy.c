// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_proxy.c
 * @brief End-to-end: the broker (localhost:1883, :8883) through an in-process HTTP CONNECT proxy.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <azure/core/az_span.h>

#include "e2e_proxy.h"
#include "test_common.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

static az_mqtt_e2e_fixture s_fixture;
static az_mqtt_proxy_options s_proxy_options;
static bool s_message_received;
static bool s_suback_received;

static bool cond_message(void) { return s_message_received; }
static bool cond_suback(void) { return s_suback_received; }

static void on_publish(AZ_MQTT_T(client)* client, AZ_MQTT_T(publish_data) const* publish)
{
  (void)client;
  (void)publish;
  s_message_received = true;
}

static void on_suback(AZ_MQTT_T(client)* client, AZ_MQTT_T(suback_data) const* suback)
{
  (void)client;
  (void)suback;
  s_suback_received = true;
}

static int64_t _now_ms(void)
{
#ifdef _WIN32
  return (int64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static az_result _init(
    AZ_MQTT_T(client)* client,
    e2e_proxy const* proxy,
    char const* host,
    uint16_t port,
    az_mqtt_tls_options const* tls)
{
  az_mqtt_e2e_fixture_reset(&s_fixture);
  s_message_received = false;
  s_suback_received = false;
  memset(&s_proxy_options, 0, sizeof(s_proxy_options));
  s_proxy_options.host = AZ_SPAN_FROM_STR("127.0.0.1");
  s_proxy_options.port = e2e_proxy_port(proxy);
  az_mqtt_e2e_client_params params;
  memset(&params, 0, sizeof(params));
  params.client_id = AZ_SPAN_FROM_STR("az-mqtt-e2e-proxy");
  params.hostname = az_span_create_from_str((char*)(uintptr_t)host);
  params.port = port;
  params.keep_alive_seconds = 30;
  params.clean_start = true;
  params.tls_options = tls;
  params.proxy_options = &s_proxy_options;
  params.on_publish = on_publish;
  params.on_suback = on_suback;
  return az_mqtt_e2e_init_client(&s_fixture, client, &params);
}

/**
 * @brief connect_start, then 20 ms process_loop calls until connected: each call must return
 * within its budget (plus scheduling slack) while the proxy replies slowly, byte by byte.
 */
static void _connect_in_short_polls(AZ_MQTT_T(client)* client)
{
  assert_int_equal(AZ_MQTT_T(client_connect_start)(client, 15000), AZ_OK);
  az_result rc = AZ_OK;
  int polls = 0;
  int64_t longest = 0;
  while (az_result_succeeded(rc)
         && AZ_MQTT_T(client_get_state)(client) == AZ_MQTT_CLIENT_STATE_CONNECTING && polls < 2000)
  {
    int64_t const start = _now_ms();
    rc = AZ_MQTT_T(client_process_loop)(client, 20);
    int64_t const took = _now_ms() - start;
    longest = took > longest ? took : longest;
    polls++;
  }
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  assert_true(polls >= 10); // The 500 ms reply spanned many polls.
  assert_true(longest < 1000);
}

/** @brief Subscribe, publish QoS 1 to the same topic, and receive it: traffic flows both ways. */
static void _roundtrip(AZ_MQTT_T(client)* client)
{
  AZ_MQTT_T(subscription) sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR("az-mqtt/e2e/proxy");
  sub.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  assert_int_equal(AZ_MQTT_T(client_subscribe)(client, &sub, 1, NULL), AZ_OK);
  assert_int_equal(az_mqtt_e2e_wait_until(client, 100, 50, cond_suback), AZ_OK);
  AZ_MQTT_T(publish_options) pub = AZ_MQTT_T(publish_options_default)();
  pub.topic = AZ_SPAN_FROM_STR("az-mqtt/e2e/proxy");
  pub.payload = AZ_SPAN_FROM_STR("through the tunnel");
  pub.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  assert_int_equal(AZ_MQTT_T(client_publish)(client, &pub, NULL), AZ_OK);
  assert_int_equal(az_mqtt_e2e_wait_until(client, 100, 50, cond_message), AZ_OK);
}

static e2e_proxy* _slow_proxy(void)
{
  e2e_proxy_options po;
  memset(&po, 0, sizeof(po));
  po.reply_delay_ms = 300;
  po.fragment_reply = true;
  po.byte_delay_ms = 3;
  e2e_proxy* proxy = e2e_proxy_start(&po);
  assert_non_null(proxy);
  return proxy;
}

static void a_plain_session_runs_through_a_slow_proxy(void** state)
{
  (void)state;
  e2e_proxy* proxy = _slow_proxy();
  AZ_MQTT_T(client) client;
  assert_int_equal(_init(&client, proxy, "127.0.0.1", 1883, NULL), AZ_OK);
  _connect_in_short_polls(&client);
  _roundtrip(&client);
  assert_int_equal(e2e_proxy_tunnels(proxy), 1);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&client), AZ_OK);
  e2e_proxy_stop(proxy);
}

static void a_tls_session_runs_through_a_slow_proxy(void** state)
{
  (void)state;
#if !defined(E2E_TLS)
  skip();
#else
#ifdef _WIN32
  char const* run_tls = getenv("AZ_MQTT_RUN_TLS_E2E"); // Same opt-in as test_ssl_win32.
  if (run_tls == NULL || strcmp(run_tls, "1") != 0)
  {
    skip();
  }
#endif
  static az_mqtt_tls_options tls;
  tls = az_mqtt_tls_options_default();
  tls.ca_cert_path = AZ_SPAN_FROM_STR(E2E_CA_CERT_PATH);
  e2e_proxy* proxy = _slow_proxy();
  AZ_MQTT_T(client) client;
  assert_int_equal(_init(&client, proxy, "localhost", 8883, &tls), AZ_OK);
  _connect_in_short_polls(&client);
  _roundtrip(&client);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&client), AZ_OK);
  e2e_proxy_stop(proxy);
#endif
}

static void a_started_connect_keeps_its_proxy(void** state)
{
  (void)state;
  e2e_proxy* proxy = _slow_proxy();
  AZ_MQTT_T(client) client;
  assert_int_equal(_init(&client, proxy, "127.0.0.1", 1883, NULL), AZ_OK);
  az_mqtt_transport* const transport = (az_mqtt_transport*)s_fixture.transport_buf.bytes;
  assert_int_equal(AZ_MQTT_T(client_connect_start)(&client, 15000), AZ_OK);
  // Cleared mid-connect: the started connect still goes through the tunnel to the broker.
  assert_int_equal(az_mqtt_transport_set_proxy(transport, NULL), AZ_OK);
  az_result rc = AZ_OK;
  for (int i = 0; i < 2000 && az_result_succeeded(rc)
       && AZ_MQTT_T(client_get_state)(&client) == AZ_MQTT_CLIENT_STATE_CONNECTING;
       i++)
  {
    rc = AZ_MQTT_T(client_process_loop)(&client, 20);
  }
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  _roundtrip(&client);
  assert_int_equal(e2e_proxy_tunnels(proxy), 1);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&client), AZ_OK);

  // The next connect is direct.
  assert_int_equal(AZ_MQTT_T(client_connect)(&client, 5000), AZ_OK);
  _roundtrip(&client);
  assert_int_equal(e2e_proxy_tunnels(proxy), 1);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&client), AZ_OK);
  e2e_proxy_stop(proxy);
}

static void a_refusing_proxy_fails_the_connect(void** state)
{
  (void)state;
  e2e_proxy_options po;
  memset(&po, 0, sizeof(po));
  po.refuse_auth = true;
  e2e_proxy* proxy = e2e_proxy_start(&po);
  assert_non_null(proxy);
  AZ_MQTT_T(client) client;
  assert_int_equal(_init(&client, proxy, "127.0.0.1", 1883, NULL), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_connect)(&client, 5000), AZ_MQTT_ERROR_PROXY_AUTH);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&client), AZ_MQTT_CLIENT_STATE_DISCONNECTED);
  assert_int_equal(e2e_proxy_tunnels(proxy), 0);
  e2e_proxy_stop(proxy);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_plain_session_runs_through_a_slow_proxy),
    cmocka_unit_test(a_tls_session_runs_through_a_slow_proxy),
    cmocka_unit_test(a_started_connect_keeps_its_proxy),
    cmocka_unit_test(a_refusing_proxy_fails_the_connect),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
