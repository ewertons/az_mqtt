// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_websocket.c
 * @brief End-to-end: MQTT over WebSockets to the broker (ws localhost:8080, wss :8081), directly
 * and through an in-process HTTP CONNECT proxy.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <azure/core/az_span.h>

#ifdef E2E_PROXY
#include "e2e_proxy.h"
#endif
#include "test_common.h"

#include <stdlib.h>
#include <string.h>

#define E2E_STR2(x) #x
#define E2E_STR(x) E2E_STR2(x)
/** @brief Per MQTT version, so both suites can run at once on one broker. */
#define E2E_WS_NAME "az-mqtt-e2e-ws-v" E2E_STR(AZ_MQTT_TEST_VERSION)

#define E2E_WS_PORT 8080
#define E2E_WSS_PORT 8081

static az_mqtt_e2e_fixture s_fixture;
static az_mqtt_websocket_options s_websocket_options;
static az_mqtt_proxy_options s_proxy_options;
static bool s_suback_received;
static int32_t s_received_size;
static int32_t s_payload_size;
static uint8_t s_payload[70000];

static bool cond_suback(void) { return s_suback_received; }
static bool cond_message(void) { return s_received_size >= 0; }

static void on_publish(AZ_MQTT_T(client)* client, AZ_MQTT_T(publish_data) const* publish)
{
  (void)client;
  s_received_size = az_span_size(publish->payload) == s_payload_size
          && memcmp(az_span_ptr(publish->payload), s_payload, (size_t)s_payload_size) == 0
      ? s_payload_size
      : 0;
}

static void on_suback(AZ_MQTT_T(client)* client, AZ_MQTT_T(suback_data) const* suback)
{
  (void)client;
  (void)suback;
  s_suback_received = true;
}

static az_result _init(
    AZ_MQTT_T(client)* client,
    char const* host,
    uint16_t port,
    az_mqtt_tls_options const* tls,
    uint16_t proxy_port)
{
  az_mqtt_e2e_fixture_reset(&s_fixture);
  s_websocket_options = az_mqtt_websocket_options_default();
  memset(&s_proxy_options, 0, sizeof(s_proxy_options));
  s_proxy_options.host = proxy_port != 0 ? AZ_SPAN_FROM_STR("127.0.0.1") : AZ_SPAN_EMPTY;
  s_proxy_options.port = proxy_port;
  az_mqtt_e2e_client_params params;
  memset(&params, 0, sizeof(params));
  params.client_id = AZ_SPAN_FROM_STR(E2E_WS_NAME);
  params.hostname = az_span_create_from_str((char*)(uintptr_t)host);
  params.port = port;
  params.keep_alive_seconds = 30;
  params.clean_start = true;
  params.tls_options = tls;
  params.proxy_options = &s_proxy_options;
  params.websocket_options = &s_websocket_options;
  params.on_publish = on_publish;
  params.on_suback = on_suback;
  return az_mqtt_e2e_init_client(&s_fixture, client, &params);
}

/**
 * @brief Subscribe, then publish QoS 1 messages to the same topic and receive them intact: one
 * over AZ_MQTT_WEBSOCKET_SEND_CHUNK, one needing a 64-bit frame length.
 */
static void _roundtrip(AZ_MQTT_T(client)* client)
{
  for (size_t i = 0; i < sizeof(s_payload); i++)
  {
    s_payload[i] = (uint8_t)(i * 7);
  }
  s_suback_received = false;
  AZ_MQTT_T(subscription) sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR(E2E_WS_NAME);
  sub.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  assert_int_equal(AZ_MQTT_T(client_subscribe)(client, &sub, 1, NULL), AZ_OK);
  assert_int_equal(az_mqtt_e2e_wait_until(client, 100, 50, cond_suback), AZ_OK);
  int32_t const sizes[] = { 1500, (int32_t)sizeof(s_payload) };
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
  {
    s_received_size = -1;
    s_payload_size = sizes[i];
    AZ_MQTT_T(publish_options) pub = AZ_MQTT_T(publish_options_default)();
    pub.topic = AZ_SPAN_FROM_STR(E2E_WS_NAME);
    pub.payload = az_span_create(s_payload, s_payload_size);
    pub.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
    assert_int_equal(AZ_MQTT_T(client_publish)(client, &pub, NULL), AZ_OK);
    assert_int_equal(az_mqtt_e2e_wait_until(client, 100, 50, cond_message), AZ_OK);
    assert_int_equal(s_received_size, s_payload_size);
  }
}

/** @brief Whether TLS e2e tests run here (Windows: opt-in, as test_ssl_win32). */
static bool _tls_enabled(void)
{
#if !defined(E2E_TLS)
  return false;
#elif defined(_WIN32)
  char const* run_tls = getenv("AZ_MQTT_RUN_TLS_E2E");
  return run_tls != NULL && strcmp(run_tls, "1") == 0;
#else
  return true;
#endif
}

static az_mqtt_tls_options const* _tls(void)
{
  static az_mqtt_tls_options tls;
  tls = az_mqtt_tls_options_default();
#ifdef E2E_CA_CERT_PATH
  tls.ca_cert_path = AZ_SPAN_FROM_STR(E2E_CA_CERT_PATH);
#endif
  return &tls;
}

/** @brief In 20 ms polls, then traffic: the upgrade completes across process_loop calls. */
static void _connect_in_short_polls_and_roundtrip(AZ_MQTT_T(client)* client)
{
  assert_int_equal(AZ_MQTT_T(client_connect_start)(client, 15000), AZ_OK);
  az_result rc = AZ_OK;
  for (int i = 0; i < 2000 && az_result_succeeded(rc)
       && AZ_MQTT_T(client_get_state)(client) == AZ_MQTT_CLIENT_STATE_CONNECTING;
       i++)
  {
    rc = AZ_MQTT_T(client_process_loop)(client, 20);
  }
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  _roundtrip(client);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(client), AZ_OK);
}

static void a_session_runs_over_websockets(void** state)
{
  (void)state;
  AZ_MQTT_T(client) client;
  assert_int_equal(_init(&client, "127.0.0.1", E2E_WS_PORT, NULL, 0), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_connect)(&client, 5000), AZ_OK);
  _roundtrip(&client);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&client), AZ_OK);

  // Again on the same options, in short polls: each connect upgrades afresh.
  _connect_in_short_polls_and_roundtrip(&client);
}

static void a_session_runs_over_secure_websockets(void** state)
{
  (void)state;
  if (!_tls_enabled())
  {
    skip();
  }
  AZ_MQTT_T(client) client;
  assert_int_equal(_init(&client, "localhost", E2E_WSS_PORT, _tls(), 0), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_connect)(&client, 5000), AZ_OK);
  _roundtrip(&client);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&client), AZ_OK);
}

#ifdef E2E_PROXY
static e2e_proxy* _slow_proxy(void)
{
  e2e_proxy_options po;
  memset(&po, 0, sizeof(po));
  po.reply_delay_ms = 100;
  po.fragment_reply = true;
  po.byte_delay_ms = 2;
  e2e_proxy* proxy = e2e_proxy_start(&po);
  assert_non_null(proxy);
  return proxy;
}
#endif

static void websockets_run_through_a_proxy(void** state)
{
  (void)state;
#ifndef E2E_PROXY
  skip();
#else
  e2e_proxy* proxy = _slow_proxy();
  AZ_MQTT_T(client) client;
  assert_int_equal(
      _init(&client, "127.0.0.1", E2E_WS_PORT, NULL, e2e_proxy_port(proxy)), AZ_OK);
  _connect_in_short_polls_and_roundtrip(&client);
  if (_tls_enabled())
  {
    assert_int_equal(
        _init(&client, "localhost", E2E_WSS_PORT, _tls(), e2e_proxy_port(proxy)), AZ_OK);
    _connect_in_short_polls_and_roundtrip(&client);
  }
  assert_int_equal(e2e_proxy_tunnels(proxy), _tls_enabled() ? 2 : 1);
  e2e_proxy_stop(proxy);
#endif
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_session_runs_over_websockets),
    cmocka_unit_test(a_session_runs_over_secure_websockets),
    cmocka_unit_test(websockets_run_through_a_proxy),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
