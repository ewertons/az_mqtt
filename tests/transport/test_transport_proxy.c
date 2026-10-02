// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_transport_proxy.c
 * @brief Connecting through an HTTP CONNECT proxy, against in-process peers.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include "test_native_errors.h"
#include "test_proxy.h"
#include "test_server.h"

#if defined(AZ_MQTT_TEST_BACKEND_NONE)
#define TLS_BACKEND 0
#else
#define TLS_BACKEND 1
#endif

static az_span _str(char const* s) { return az_span_create_from_str((char*)(uintptr_t)s); }

typedef struct
{
  test_server* server;
  test_proxy* proxy;
  az_mqtt_transport* transport;
  az_mqtt_proxy_options proxy_options;
  test_native_errors errors;
} fixture;

/** @brief Subject alternative names of the next _setup()'s server certificate; NULL: default. */
static char const* s_san;

static void _setup(fixture* f, bool tls, test_proxy_options const* po)
{
  memset(f, 0, sizeof(*f));
  test_server_options so = test_server_options_default();
  so.tls = tls;
  if (s_san != NULL)
  {
    so.san = s_san;
    s_san = NULL;
  }
  f->server = test_server_start(&so);
  assert_non_null(f->server);
  f->proxy = test_proxy_start(po);
  assert_non_null(f->proxy);
  f->transport = (az_mqtt_transport*)calloc(1, (size_t)az_mqtt_transport_sizeof());
  assert_non_null(f->transport);
  assert_int_equal(az_mqtt_transport_init(f->transport), AZ_OK);
  f->proxy_options.host = _str("127.0.0.1");
  f->proxy_options.port = test_proxy_port(f->proxy);
  assert_int_equal(az_mqtt_transport_set_proxy(f->transport, &f->proxy_options), AZ_OK);
  az_mqtt_transport_set_error_callback(f->transport, test_native_errors_record, &f->errors);
}

static void _teardown(fixture* f)
{
  az_mqtt_transport_close(f->transport);
  free(f->transport);
  test_proxy_stop(f->proxy);
  test_server_stop(f->server);
}

static az_mqtt_tls_options _trusting(fixture const* f)
{
  az_mqtt_tls_options t = az_mqtt_tls_options_default();
  t.ca_cert_path = _str(test_server_ca_path(f->server));
  return t;
}

static az_result _connect(fixture* f, char const* host, az_mqtt_tls_options const* tls)
{
  return az_mqtt_transport_connect(f->transport, _str(host), test_server_port(f->server), tls);
}

/** @brief Send CONNECT and wait for the CONNACK. */
static void _mqtt_handshake(az_mqtt_transport* t)
{
  static uint8_t connect_v5[] = { 0x10, 0x0E, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x05,
                                  0x02, 0x00, 0x3C, 0x00, 0x00, 0x01, 'c' };
  assert_int_equal(az_mqtt_transport_send(t, AZ_SPAN_FROM_BUFFER(connect_v5)), AZ_OK);
  uint8_t buf[8];
  az_span got = AZ_SPAN_EMPTY;
  assert_int_equal(az_mqtt_transport_receive(t, AZ_SPAN_FROM_BUFFER(buf), 3000, &got), AZ_OK);
  assert_true(az_span_size(got) > 0);
  assert_int_equal(buf[0], 0x20);
}

static void a_plain_connection_goes_through_the_tunnel(void** state)
{
  (void)state;
  test_proxy_options po = { 0 };
  fixture f;
  _setup(&f, false, &po);
  assert_int_equal(_connect(&f, "127.0.0.1", NULL), AZ_OK);
  _mqtt_handshake(f.transport);
  char line[128];
  char expected[128];
  test_proxy_last_request_line(f.proxy, line, sizeof(line));
  snprintf(expected, sizeof(expected), "CONNECT 127.0.0.1:%u HTTP/1.1", test_server_port(f.server));
  assert_string_equal(line, expected);
  assert_int_equal(test_proxy_tunnels(f.proxy), 1);
  assert_int_equal(test_server_accepted(f.server), 1);
  assert_int_equal(f.errors.count, 0);
  _teardown(&f);
}

static void tls_in_the_tunnel_verifies_the_server_not_the_proxy(void** state)
{
  (void)state;
  if (!TLS_BACKEND)
  {
    skip();
  }
  test_proxy_options po = { 0 };
  s_san = "DNS:localhost";
  fixture f;
  _setup(&f, true, &po);
  az_mqtt_tls_options const t = _trusting(&f);
  assert_int_equal(_connect(&f, "localhost", &t), AZ_OK); // The certificate names localhost only.
  _mqtt_handshake(f.transport);
  assert_int_equal(test_server_handshakes(f.server), 1);
  az_mqtt_transport_close(f.transport);

  // A name the certificate lacks is refused, as without a proxy.
  test_native_errors_clear(&f.errors);
  assert_int_equal(_connect(&f, "127.0.0.1", &t), AZ_MQTT_ERROR_TLS_VERIFY);
  assert_non_null(test_native_errors_first_of(&f.errors, AZ_MQTT_ERROR_TLS_VERIFY));
  _teardown(&f);
}

static void basic_credentials_are_sent_verbatim(void** state)
{
  (void)state;
  test_proxy_options po = { 0 };
  po.username = "dev@corp";
  po.password = "p@ss%77rd:x";
  fixture f;
  _setup(&f, false, &po);
  f.proxy_options.username = _str("dev@corp");
  f.proxy_options.password = _str("p@ss%77rd:x");
  assert_int_equal(az_mqtt_transport_set_proxy(f.transport, &f.proxy_options), AZ_OK);
  assert_int_equal(_connect(&f, "127.0.0.1", NULL), AZ_OK);
  _mqtt_handshake(f.transport);
  assert_int_equal(test_proxy_auth_failures(f.proxy), 0);
  assert_int_equal(test_proxy_tunnels(f.proxy), 1);
  _teardown(&f);
}

static void wrong_or_missing_credentials_are_a_proxy_auth_error(void** state)
{
  (void)state;
  test_proxy_options po = { 0 };
  po.username = "device";
  po.password = "right";
  fixture f;
  _setup(&f, false, &po);
  for (int wrong = 0; wrong <= 1; wrong++)
  {
    f.proxy_options.username = wrong ? _str("device") : AZ_SPAN_EMPTY;
    f.proxy_options.password = wrong ? _str("wrong") : AZ_SPAN_EMPTY;
    assert_int_equal(az_mqtt_transport_set_proxy(f.transport, &f.proxy_options), AZ_OK);
    test_native_errors_clear(&f.errors);
    assert_int_equal(_connect(&f, "127.0.0.1", NULL), AZ_MQTT_ERROR_PROXY_AUTH);
    assert_int_equal(f.errors.count, 1);
    assert_int_equal(f.errors.errors[0].source, AZ_MQTT_NATIVE_ERROR_PROXY);
    assert_int_equal(f.errors.errors[0].code, 407);
    assert_int_equal(f.errors.errors[0].result, AZ_MQTT_ERROR_PROXY_AUTH);
  }
  assert_int_equal(test_proxy_auth_failures(f.proxy), 2);
  assert_int_equal(test_server_accepted(f.server), 0); // Never reached directly.
  _teardown(&f);
}

static void a_refused_tunnel_is_a_proxy_error(void** state)
{
  (void)state;
  int const statuses[] = { 403, 502 };
  for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++)
  {
    test_proxy_options po = { 0 };
    po.status = statuses[i];
    fixture f;
    _setup(&f, false, &po);
    assert_int_equal(_connect(&f, "127.0.0.1", NULL), AZ_MQTT_ERROR_PROXY);
    assert_int_equal(f.errors.count, 1);
    assert_int_equal(f.errors.errors[0].source, AZ_MQTT_NATIVE_ERROR_PROXY);
    assert_int_equal(f.errors.errors[0].code, statuses[i]);
    assert_int_equal(test_server_accepted(f.server), 0);
    _teardown(&f);
  }
}

static void no_reply_or_a_non_http_reply_is_a_proxy_error(void** state)
{
  (void)state;
  for (int garbage = 0; garbage <= 1; garbage++)
  {
    test_proxy_options po = { 0 };
    po.close_without_reply = !garbage;
    po.garbage_reply = garbage;
    fixture f;
    _setup(&f, false, &po);
    assert_int_equal(_connect(&f, "127.0.0.1", NULL), AZ_MQTT_ERROR_PROXY);
    if (garbage)
    {
      assert_int_equal(f.errors.count, 1);
      assert_int_equal(f.errors.errors[0].source, AZ_MQTT_NATIVE_ERROR_PROXY);
      assert_int_equal(f.errors.errors[0].code, 0);
    }
    assert_true(test_native_errors_all_belong_to(&f.errors, AZ_MQTT_ERROR_PROXY, 1));
    assert_int_equal(test_server_accepted(f.server), 0);
    _teardown(&f);
  }
}

static void a_slow_fragmented_reply_with_long_headers_is_awaited(void** state)
{
  (void)state;
  test_proxy_options po = { 0 };
  po.fragment_reply = true;
  po.extra_header_bytes = 4000;
  po.reply_delay_ms = 200;
  fixture f;
  _setup(&f, TLS_BACKEND, &po);
  az_mqtt_tls_options const t = _trusting(&f);
  az_mqtt_tls_options const* tls = TLS_BACKEND ? &t : NULL;
  az_span const host = _str(TLS_BACKEND ? "localhost" : "127.0.0.1");
  assert_int_equal(
      az_mqtt_transport_connect_start(f.transport, host, test_server_port(f.server), tls), AZ_OK);
  int timeouts = 0;
  az_result rc = AZ_MQTT_ERROR_TIMEOUT;
  for (int i = 0; i < 1000 && rc == AZ_MQTT_ERROR_TIMEOUT; i++)
  {
    rc = az_mqtt_transport_connect_poll(f.transport, 20); // Resumes mid-reply.
    timeouts += rc == AZ_MQTT_ERROR_TIMEOUT;
  }
  assert_int_equal(rc, AZ_OK);
  assert_true(timeouts > 1);
  _mqtt_handshake(f.transport); // Nothing of the tunnelled stream was taken with the reply.
  _teardown(&f);
}

static void an_unreachable_proxy_is_never_bypassed(void** state)
{
  (void)state;
  test_proxy_options po = { 0 };
  fixture f;
  _setup(&f, false, &po);
  test_proxy_stop(f.proxy); // Its port now refuses connections.
  f.proxy = NULL;
  assert_int_equal(_connect(&f, "127.0.0.1", NULL), AZ_MQTT_ERROR_CONNECTION_REFUSED);
  assert_int_equal(test_server_accepted(f.server), 0);
  _teardown(&f);
}

static void a_started_connect_keeps_its_proxy(void** state)
{
  (void)state;
  test_proxy_options po = { 0 };
  fixture f;
  _setup(&f, false, &po);
  az_span const host = _str("127.0.0.1");
  assert_int_equal(
      az_mqtt_transport_connect_start(f.transport, host, test_server_port(f.server), NULL), AZ_OK);
  assert_int_equal(az_mqtt_transport_set_proxy(f.transport, NULL), AZ_OK); // For the next one.
  az_result rc = AZ_MQTT_ERROR_TIMEOUT;
  for (int i = 0; i < 300 && rc == AZ_MQTT_ERROR_TIMEOUT; i++)
  {
    rc = az_mqtt_transport_connect_poll(f.transport, 10);
  }
  assert_int_equal(rc, AZ_OK);
  _mqtt_handshake(f.transport); // Through the tunnel, not to the proxy itself.
  assert_int_equal(test_proxy_tunnels(f.proxy), 1);
  az_mqtt_transport_close(f.transport);

  assert_int_equal(_connect(&f, "127.0.0.1", NULL), AZ_OK); // Now directly.
  _mqtt_handshake(f.transport);
  assert_int_equal(test_proxy_requests(f.proxy), 1);
  _teardown(&f);
}

static void the_proxy_can_be_set_invalid_or_cleared(void** state)
{
  (void)state;
  test_proxy_options po = { 0 };
  fixture f;
  _setup(&f, false, &po);
  az_mqtt_proxy_options bad = f.proxy_options;
  bad.username = _str("user\r\nX-Injected: 1");
  assert_int_equal(az_mqtt_transport_set_proxy(f.transport, &bad), AZ_MQTT_ERROR_INVALID_CONFIG);
  bad = f.proxy_options;
  bad.port = 0;
  assert_int_equal(az_mqtt_transport_set_proxy(f.transport, &bad), AZ_MQTT_ERROR_INVALID_CONFIG);

  // The valid proxy set before is still in use.
  assert_int_equal(_connect(&f, "127.0.0.1", NULL), AZ_OK);
  assert_int_equal(test_proxy_tunnels(f.proxy), 1);
  az_mqtt_transport_close(f.transport);

  // A target that cannot go in a request line is refused before connecting.
  assert_int_equal(_connect(&f, "127.0.0.1\r\nX: 1", NULL), AZ_MQTT_ERROR_INVALID_CONFIG);
  assert_int_equal(test_proxy_requests(f.proxy), 1);

  // Cleared: direct again.
  assert_int_equal(az_mqtt_transport_set_proxy(f.transport, NULL), AZ_OK);
  assert_int_equal(_connect(&f, "127.0.0.1", NULL), AZ_OK);
  _mqtt_handshake(f.transport);
  assert_int_equal(test_proxy_requests(f.proxy), 1);
  _teardown(&f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_plain_connection_goes_through_the_tunnel),
    cmocka_unit_test(tls_in_the_tunnel_verifies_the_server_not_the_proxy),
    cmocka_unit_test(basic_credentials_are_sent_verbatim),
    cmocka_unit_test(wrong_or_missing_credentials_are_a_proxy_auth_error),
    cmocka_unit_test(a_refused_tunnel_is_a_proxy_error),
    cmocka_unit_test(no_reply_or_a_non_http_reply_is_a_proxy_error),
    cmocka_unit_test(a_slow_fragmented_reply_with_long_headers_is_awaited),
    cmocka_unit_test(an_unreachable_proxy_is_never_bypassed),
    cmocka_unit_test(a_started_connect_keeps_its_proxy),
    cmocka_unit_test(the_proxy_can_be_set_invalid_or_cleared),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
