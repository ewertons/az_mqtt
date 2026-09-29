// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_transport_robustness.c
 * @brief Bounded connect, receive timeouts and peer resets, against an in-process peer.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cmocka.h>

#include <az_mqtt5/az_mqtt5_transport.h>
#include <az_mqtt5/az_mqtt5_types.h>

#include "test_server.h"

#if defined(AZ_MQTT5_TEST_BACKEND_NONE)
#define TLS_BACKEND 0
#else
#define TLS_BACKEND 1
#endif

static int64_t _now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void _sleep_ms(int ms)
{
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}

static az_mqtt5_transport* _transport_new(void)
{
  az_mqtt5_transport* t = (az_mqtt5_transport*)calloc(1, (size_t)az_mqtt5_transport_sizeof());
  assert_non_null(t);
  assert_int_equal(az_mqtt5_transport_init(t), AZ_OK);
  return t;
}

static void _transport_free(az_mqtt5_transport* t)
{
  az_mqtt5_transport_close(t);
  free(t);
}

static az_span _str(char const* s) { return az_span_create_from_str((char*)(uintptr_t)s); }

static az_mqtt5_tls_options _trusting(test_server const* s)
{
  az_mqtt5_tls_options t = az_mqtt5_tls_options_default();
  t.ca_cert_path = _str(test_server_ca_path(s));
  return t;
}

/** @brief Send CONNECT and wait for the CONNACK. */
static void _mqtt_handshake(az_mqtt5_transport* t)
{
  static uint8_t connect_v5[] = { 0x10, 0x0E, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x05,
                                  0x02, 0x00, 0x3C, 0x00, 0x00, 0x01, 'c' };
  assert_int_equal(az_mqtt5_transport_send(t, AZ_SPAN_FROM_BUFFER(connect_v5)), AZ_OK);
  uint8_t buf[8];
  az_span got = AZ_SPAN_EMPTY;
  assert_int_equal(az_mqtt5_transport_receive(t, AZ_SPAN_FROM_BUFFER(buf), 3000, &got), AZ_OK);
  assert_true(az_span_size(got) > 0);
  assert_int_equal(buf[0], 0x20);
}

// ──────────────────────── Bounded connect ────────────────────

static void tls_handshake_with_a_silent_peer_times_out(void** state)
{
  (void)state;
  if (!TLS_BACKEND)
  {
    skip();
  }
  test_server_options o = test_server_options_default();
  o.behavior = TEST_SERVER_SILENT;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  az_mqtt5_transport* t = _transport_new();
  az_mqtt5_tls_options tls = _trusting(s);

  assert_int_equal(
      az_mqtt5_transport_connect_start(t, _str("127.0.0.1"), test_server_port(s), &tls), AZ_OK);
  for (int i = 0; i < 2; i++)
  {
    int64_t t0 = _now_ms();
    assert_int_equal(az_mqtt5_transport_connect_poll(t, 300), AZ_MQTT5_ERROR_TIMEOUT);
    int64_t elapsed = _now_ms() - t0;
    assert_true(elapsed >= 250 && elapsed < 1500);
  }
  assert_int_equal(test_server_accepted(s), 1);

  _transport_free(t);
  test_server_stop(s);
}

static void tcp_connect_to_a_full_backlog_times_out(void** state)
{
  (void)state;
  // A listener that never accepts, with its backlog already full, drops SYNs:
  // the same as an address that never answers.
  int l = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t alen = sizeof(a);
  assert_int_equal(bind(l, (struct sockaddr*)&a, sizeof(a)), 0);
  assert_int_equal(listen(l, 0), 0);
  assert_int_equal(getsockname(l, (struct sockaddr*)&a, &alen), 0);
  int fillers[2];
  for (int i = 0; i < 2; i++)
  {
    fillers[i] = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(fillers[i], F_SETFL, O_NONBLOCK);
    (void)connect(fillers[i], (struct sockaddr*)&a, sizeof(a));
  }
  _sleep_ms(50);

  az_mqtt5_transport* t = _transport_new();
  assert_int_equal(az_mqtt5_transport_connect_start(t, _str("127.0.0.1"), ntohs(a.sin_port), NULL), AZ_OK);
  int64_t t0 = _now_ms();
  assert_int_equal(az_mqtt5_transport_connect_poll(t, 300), AZ_MQTT5_ERROR_TIMEOUT);
  assert_true(_now_ms() - t0 < 1500);

  _transport_free(t);
  close(fillers[0]);
  close(fillers[1]);
  close(l);
}

static void connect_poll_completes_a_plain_connection(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.tls = false;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  az_mqtt5_transport* t = _transport_new();

  // "localhost" may resolve to ::1 first, where nothing listens: the next address must be tried.
  assert_int_equal(az_mqtt5_transport_connect_start(t, _str("localhost"), test_server_port(s), NULL), AZ_OK);
  az_result rc;
  int64_t const deadline = _now_ms() + 3000;
  while ((rc = az_mqtt5_transport_connect_poll(t, 50)) == AZ_MQTT5_ERROR_TIMEOUT
         && _now_ms() < deadline)
  {
  }
  assert_int_equal(rc, AZ_OK);
  _mqtt_handshake(t);

  _transport_free(t);
  test_server_stop(s);
}

static void connect_to_a_closed_port_fails(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.tls = false;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  uint16_t port = test_server_port(s);
  test_server_stop(s);

  az_mqtt5_transport* t = _transport_new();
  az_result rc = az_mqtt5_transport_connect(t, _str("127.0.0.1"), port, NULL);
  assert_int_equal(rc, AZ_MQTT5_ERROR_TRANSPORT);
  _transport_free(t);
}

// ──────────────────────── I/O ────────────────────────────────

static void _receive_waits_for_the_timeout(bool tls)
{
  test_server_options o = test_server_options_default();
  o.tls = tls;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  az_mqtt5_transport* t = _transport_new();
  az_mqtt5_tls_options topt = _trusting(s);
  assert_int_equal(
      az_mqtt5_transport_connect(t, _str("localhost"), test_server_port(s), tls ? &topt : NULL),
      AZ_OK);
  _mqtt_handshake(t);

  uint8_t buf[16];
  az_span got = AZ_SPAN_EMPTY;
  int64_t t0 = _now_ms();
  assert_int_equal(az_mqtt5_transport_receive(t, AZ_SPAN_FROM_BUFFER(buf), 300, &got), AZ_OK);
  int64_t elapsed = _now_ms() - t0;
  assert_int_equal(az_span_size(got), 0);
  assert_true(elapsed >= 250 && elapsed < 1500);

  assert_int_equal(az_mqtt5_transport_receive(t, AZ_SPAN_FROM_BUFFER(buf), 0, &got), AZ_OK);
  assert_int_equal(az_span_size(got), 0);

  _transport_free(t);
  test_server_stop(s);
}

static void plain_receive_waits_for_the_timeout(void** state)
{
  (void)state;
  _receive_waits_for_the_timeout(false);
}

static void tls_receive_waits_for_the_timeout(void** state)
{
  (void)state;
  if (!TLS_BACKEND)
  {
    skip();
  }
  _receive_waits_for_the_timeout(true);
}

/**
 * @brief Writing to a connection the peer has closed must return an error, not
 * kill the process with SIGPIPE (SIGPIPE is left at its default action here).
 */
static void _peer_close_is_an_error_not_a_signal(bool tls)
{
  test_server_options o = test_server_options_default();
  o.tls = tls;
  o.behavior = TEST_SERVER_CLOSE_AFTER_CONNACK;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  az_mqtt5_transport* t = _transport_new();
  az_mqtt5_tls_options topt = _trusting(s);
  assert_int_equal(
      az_mqtt5_transport_connect(t, _str("localhost"), test_server_port(s), tls ? &topt : NULL),
      AZ_OK);
  _mqtt_handshake(t);
  _sleep_ms(400);

  static uint8_t chunk[16 * 1024];
  az_result rc = AZ_OK;
  for (int i = 0; i < 64 && az_result_succeeded(rc); i++)
  {
    rc = az_mqtt5_transport_send(t, AZ_SPAN_FROM_BUFFER(chunk));
  }
  assert_true(az_result_failed(rc));

  uint8_t buf[16];
  az_span got = AZ_SPAN_EMPTY;
  assert_true(az_result_failed(az_mqtt5_transport_receive(t, AZ_SPAN_FROM_BUFFER(buf), 100, &got)));

  _transport_free(t);
  test_server_stop(s);
}

static void plain_peer_close_is_an_error_not_a_signal(void** state)
{
  (void)state;
  _peer_close_is_an_error_not_a_signal(false);
}

static void tls_peer_close_is_an_error_not_a_signal(void** state)
{
  (void)state;
  if (!TLS_BACKEND)
  {
    skip();
  }
  _peer_close_is_an_error_not_a_signal(true);
}

int main(void)
{
  signal(SIGPIPE, SIG_DFL);
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(tls_handshake_with_a_silent_peer_times_out),
    cmocka_unit_test(tcp_connect_to_a_full_backlog_times_out),
    cmocka_unit_test(connect_poll_completes_a_plain_connection),
    cmocka_unit_test(connect_to_a_closed_port_fails),
    cmocka_unit_test(plain_receive_waits_for_the_timeout),
    cmocka_unit_test(tls_receive_waits_for_the_timeout),
    cmocka_unit_test(plain_peer_close_is_an_error_not_a_signal),
    cmocka_unit_test(tls_peer_close_is_an_error_not_a_signal),
  };
  return cmocka_run_group_tests_name("transport_robustness", tests, NULL, NULL);
}
