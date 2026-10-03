// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_proxy_transport.c
 * @brief Unit tests: the HTTP CONNECT proxy layer over an in-memory transport.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "az_mqtt_io_layers_internal.h"
#include "test_fake_transport.h"

#include <string.h>

typedef struct
{
  test_fake_transport fake;
  _az_mqtt_proxy_transport proxy;
  az_mqtt_transport* t;
  az_mqtt_proxy_options options;
} fixture;

static uint8_t s_input[4096];
static uint8_t s_sent[4096];

static struct
{
  az_mqtt_native_error errors[8];
  int count;
} s_native;

static void _on_native(az_mqtt_native_error const* error, void* context)
{
  (void)context;
  if (s_native.count < 8)
  {
    s_native.errors[s_native.count] = *error;
  }
  s_native.count++;
}

/** @brief Proxy layer over a fresh fake; proxy 10.0.0.1:3128 unless @p direct. */
static void _init(fixture* f, bool direct)
{
  memset(s_input, 0, sizeof(s_input));
  memset(s_sent, 0, sizeof(s_sent));
  memset(&s_native, 0, sizeof(s_native));
  test_fake_transport_init(
      &f->fake, AZ_SPAN_FROM_BUFFER(s_input), az_span_create(s_sent, (int32_t)sizeof(s_sent) - 1));
  assert_int_equal(_az_mqtt_proxy_transport_init(&f->proxy, &f->fake.layer), AZ_OK);
  f->t = &f->proxy.layer.base;
  az_mqtt_transport_set_error_callback(f->t, _on_native, NULL);
  memset(&f->options, 0, sizeof(f->options));
  f->options.host = AZ_SPAN_FROM_STR("10.0.0.1");
  f->options.port = 3128;
  f->options.username = AZ_SPAN_FROM_STR("user");
  f->options.password = AZ_SPAN_FROM_STR("pass");
  assert_int_equal(az_mqtt_transport_set_proxy(f->t, direct ? NULL : &f->options), AZ_OK);
}

static void _feed(fixture* f, char const* text)
{
  test_fake_transport_feed(&f->fake, text, (int32_t)strlen(text));
}

/** @brief Poll with no wait until not AZ_MQTT_ERROR_TIMEOUT. */
static az_result _poll(fixture* f)
{
  az_result rc = AZ_MQTT_ERROR_TIMEOUT;
  for (int i = 0; i < 10000 && rc == AZ_MQTT_ERROR_TIMEOUT; i++)
  {
    rc = az_mqtt_transport_connect_poll(f->t, 0);
  }
  return rc;
}

static void without_a_proxy_bytes_pass_through(void** state)
{
  (void)state;
  fixture f;
  _init(&f, true);
  az_mqtt_tls_options const tls = az_mqtt_tls_options_default();
  assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 8883, &tls), AZ_OK);
  assert_true(az_span_is_content_equal(f.fake.host, AZ_SPAN_FROM_STR("hub")));
  assert_int_equal(f.fake.port, 8883);
  assert_ptr_equal(f.fake.tls_options, &tls); // Passed down, never dropped.
  assert_int_equal(_poll(&f), AZ_OK);
  assert_int_equal(f.fake.sent_size, 0);
  _feed(&f, "abc");
  uint8_t buffer[8];
  az_span received;
  assert_int_equal(az_mqtt_transport_receive(f.t, AZ_SPAN_FROM_BUFFER(buffer), 0, &received), AZ_OK);
  assert_int_equal(az_span_size(received), 3);
}

static void the_tunnel_opens_in_any_pieces_and_keeps_what_follows(void** state)
{
  (void)state;
  static char const reply[] = "HTTP/1.1 200 Connection established\r\nVia: x\r\n\r\n\x16\x03\x01";
  for (int32_t chunk = 1; chunk <= (int32_t)sizeof(reply); chunk++)
  {
    fixture f;
    _init(&f, false);
    assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 8883, NULL), AZ_OK);
    assert_true(az_span_is_content_equal(f.fake.host, AZ_SPAN_FROM_STR("10.0.0.1")));
    assert_int_equal(f.fake.port, 3128);
    f.fake.connect_polls_pending = 1;
    assert_int_equal(az_mqtt_transport_connect_poll(f.t, 0), AZ_MQTT_ERROR_TIMEOUT);
    assert_int_equal(az_mqtt_transport_connect_poll(f.t, 0), AZ_MQTT_ERROR_TIMEOUT); // Sent; no reply.
    assert_non_null(strstr((char const*)s_sent, "CONNECT hub:8883 HTTP/1.1\r\nHost: hub:8883\r\n"));
    assert_non_null(strstr((char const*)s_sent, "\r\nProxy-Authorization: Basic dXNlcjpwYXNz\r\n"));
    f.fake.chunk = chunk;
    _feed(&f, reply);
    assert_int_equal(_poll(&f), AZ_OK);
    // The bytes after the reply are the tunnelled stream's.
    uint8_t got[8];
    int32_t size = 0;
    for (int i = 0; i < 10 && size < 3; i++)
    {
      az_span received;
      assert_int_equal(
          az_mqtt_transport_receive(f.t, az_span_create(got + size, 8 - size), 0, &received), AZ_OK);
      size += az_span_size(received);
    }
    assert_int_equal(size, 3);
    assert_memory_equal(got, "\x16\x03\x01", 3);
    assert_int_equal(s_native.count, 0);
  }
}

static void the_request_is_sent_across_polls_without_blocking(void** state)
{
  (void)state;
  fixture f;
  _init(&f, false);
  assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 8883, NULL), AZ_OK);
  f.fake.send_some_budget = 0; // The connection takes nothing for now.
  for (int i = 0; i < 3; i++)
  {
    assert_int_equal(az_mqtt_transport_connect_poll(f.t, 0), AZ_MQTT_ERROR_TIMEOUT);
  }
  assert_int_equal(f.fake.sent_size, 0);
  int polls = 0;
  while (strstr((char const*)s_sent, "\r\n\r\n") == NULL && polls < 1000)
  {
    f.fake.send_some_budget = 7; // A few bytes of room per poll.
    assert_int_equal(az_mqtt_transport_connect_poll(f.t, 0), AZ_MQTT_ERROR_TIMEOUT);
    polls++;
  }
  assert_true(polls > 2);
  assert_non_null(strstr((char const*)s_sent, "CONNECT hub:8883 HTTP/1.1\r\nHost: hub:8883\r\n"));
  _feed(&f, "HTTP/1.1 200 OK\r\n\r\n");
  assert_int_equal(_poll(&f), AZ_OK);
  // Partial sends pass through once open.
  f.fake.send_some_budget = 7;
  int32_t sent = 0;
  assert_int_equal(
      _az_mqtt_io_layer_send_some(&f.proxy.layer, AZ_SPAN_FROM_STR("0123456789"), 0, &sent), AZ_OK);
  assert_int_equal(sent, 7);
}

static void refusals_are_reported_with_the_status(void** state)
{
  (void)state;
  struct
  {
    char const* reply;
    az_result rc;
    int32_t status;
  } const cases[] = {
    { "HTTP/1.1 407 Proxy Authentication Required\r\n\r\n", AZ_MQTT_ERROR_PROXY_AUTH, 407 },
    { "HTTP/1.1 403 Forbidden\r\n\r\n", AZ_MQTT_ERROR_PROXY, 403 },
    { "SSH-2.0-OpenSSH\r\n\r\n", AZ_MQTT_ERROR_PROXY, 0 },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    fixture f;
    _init(&f, false);
    assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 8883, NULL), AZ_OK);
    _feed(&f, cases[i].reply);
    assert_int_equal(_poll(&f), cases[i].rc);
    assert_int_equal(f.fake.closes, 1);
    assert_int_equal(s_native.count, 1);
    assert_int_equal(s_native.errors[0].source, AZ_MQTT_NATIVE_ERROR_PROXY);
    assert_int_equal(s_native.errors[0].code, cases[i].status);
    assert_int_equal(s_native.errors[0].result, cases[i].rc);
    assert_int_equal(s_native.errors[0].connect_attempt, 1);
  }
}

/** @brief A native error from below, as the socket would report it. */
static void _lower_error(fixture* f, az_result result)
{
  az_mqtt_native_error const e = { AZ_MQTT_NATIVE_ERROR_SOCKET, 104, result, 99 };
  f->fake.error_callback(&e, f->fake.error_context);
}

static void failures_below_during_the_exchange_are_proxy_failures(void** state)
{
  (void)state;
  // Closed before the reply.
  fixture f;
  _init(&f, false);
  assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 8883, NULL), AZ_OK);
  f.fake.end_of_input = AZ_MQTT_ERROR_CONNECTION_CLOSED;
  _feed(&f, "HTTP/1.1 200");
  assert_int_equal(_poll(&f), AZ_MQTT_ERROR_PROXY);
  assert_int_equal(f.fake.closes, 1);

  // Sending the request fails; its socket error is the proxy's.
  _init(&f, false);
  assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 8883, NULL), AZ_OK);
  f.fake.fail_send_call = 1;
  f.fake.failure_errno = 32;
  assert_int_equal(_poll(&f), AZ_MQTT_ERROR_PROXY);
  assert_int_equal(s_native.count, 1);
  assert_int_equal(s_native.errors[0].source, AZ_MQTT_NATIVE_ERROR_SOCKET);
  assert_int_equal(s_native.errors[0].code, 32);
  assert_int_equal(s_native.errors[0].result, AZ_MQTT_ERROR_PROXY);
  assert_int_equal(s_native.errors[0].connect_attempt, 1);

  // Reading the reply fails.
  _init(&f, false);
  assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 8883, NULL), AZ_OK);
  f.fake.end_of_input = AZ_MQTT_ERROR_TRANSPORT;
  f.fake.failure_errno = 5;
  assert_int_equal(_poll(&f), AZ_MQTT_ERROR_PROXY);
  assert_int_equal(s_native.count, 1);
  assert_int_equal(s_native.errors[0].result, AZ_MQTT_ERROR_PROXY);

  // After the exchange, errors from below keep their own result.
  _init(&f, false);
  assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 8883, NULL), AZ_OK);
  _feed(&f, "HTTP/1.1 200 OK\r\n\r\n");
  assert_int_equal(_poll(&f), AZ_OK);
  _lower_error(&f, AZ_MQTT_ERROR_CONNECTION_CLOSED);
  assert_int_equal(s_native.errors[0].result, AZ_MQTT_ERROR_CONNECTION_CLOSED);

  // Errors from below are stamped: this layer's attempt; the proxy's result during the exchange.
  _init(&f, false);
  assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 8883, NULL), AZ_OK);
  assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 8883, NULL), AZ_OK);
  _lower_error(&f, AZ_MQTT_ERROR_CONNECTION_REFUSED); // While connecting to the proxy.
  assert_int_equal(s_native.errors[0].result, AZ_MQTT_ERROR_CONNECTION_REFUSED);
  assert_int_equal(s_native.errors[0].connect_attempt, 2);
}

static void a_started_connect_keeps_its_proxy(void** state)
{
  (void)state;
  fixture f;
  _init(&f, false);
  assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 8883, NULL), AZ_OK);
  assert_int_equal(az_mqtt_transport_set_proxy(f.t, NULL), AZ_OK);
  _feed(&f, "HTTP/1.1 200 OK\r\n\r\n");
  assert_int_equal(_poll(&f), AZ_OK);
  assert_non_null(strstr((char const*)s_sent, "CONNECT hub:8883 "));
  // The next one is direct.
  int32_t const sent = f.fake.sent_size;
  assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 8883, NULL), AZ_OK);
  assert_true(az_span_is_content_equal(f.fake.host, AZ_SPAN_FROM_STR("hub")));
  assert_int_equal(_poll(&f), AZ_OK);
  assert_int_equal(f.fake.sent_size, sent);
}

static void invalid_settings_are_refused(void** state)
{
  (void)state;
  fixture f;
  _init(&f, false);
  assert_int_equal(
      az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub\r\nX: y"), 8883, NULL),
      AZ_MQTT_ERROR_INVALID_CONFIG);
  assert_int_equal(f.fake.connects, 0); // Before any connect.
  az_mqtt_proxy_options bad = f.options;
  bad.port = 0;
  assert_int_equal(az_mqtt_transport_set_proxy(f.t, &bad), AZ_MQTT_ERROR_INVALID_CONFIG);
  uint8_t buffer[4];
  az_span received;
  assert_int_equal(
      az_mqtt_transport_receive(f.t, AZ_SPAN_FROM_BUFFER(buffer), 0, &received),
      AZ_MQTT_ERROR_TRANSPORT);
  assert_int_equal(az_mqtt_transport_connect_poll(f.t, 0), AZ_MQTT_ERROR_INVALID_STATE);
}

static void shutdown_and_close_reach_the_transport_below(void** state)
{
  (void)state;
  fixture f;
  _init(&f, true);
  assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), 1883, NULL), AZ_OK);
  assert_int_equal(_poll(&f), AZ_OK);
  az_mqtt_transport_shutdown(f.t);
  az_mqtt_transport_close(f.t);
  assert_int_equal(f.fake.shutdowns, 1);
  assert_int_equal(f.fake.closes, 1);
  uint8_t data[1] = { 0 };
  assert_int_equal(az_mqtt_transport_send(f.t, AZ_SPAN_FROM_BUFFER(data)), AZ_MQTT_ERROR_TRANSPORT);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(without_a_proxy_bytes_pass_through),
    cmocka_unit_test(the_tunnel_opens_in_any_pieces_and_keeps_what_follows),
    cmocka_unit_test(the_request_is_sent_across_polls_without_blocking),
    cmocka_unit_test(refusals_are_reported_with_the_status),
    cmocka_unit_test(failures_below_during_the_exchange_are_proxy_failures),
    cmocka_unit_test(a_started_connect_keeps_its_proxy),
    cmocka_unit_test(invalid_settings_are_refused),
    cmocka_unit_test(shutdown_and_close_reach_the_transport_below),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
