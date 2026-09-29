// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_client_session.c
 * @brief Session lifecycle: keep-alive, teardown and callbacks, against an in-process peer.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include <az_mqtt5/az_mqtt5_client.h>

#include "test_server.h"

#define ARRAY_SPAN(a) az_span_create((uint8_t*)(a), (int32_t)sizeof(a))

typedef struct
{
  az_mqtt5_client client;
  az_mqtt5_transport* transport;
  test_server* server;
  uint8_t send_buf[1024];
  uint8_t recv_buf[1024];
  az_mqtt5_user_property props[4][4];
  int32_t sub_ids[4];
  az_mqtt5_reason_code reasons[4];
} fixture;

static struct
{
  int connacks;
  int connack_reason;
  int subacks;
  int32_t suback_count;
  int publishes;
  int disconnects;
  int closed;
  az_result closed_reason;
} g;

static void _on_connack(az_mqtt5_client* c, az_mqtt5_connack_data const* d)
{
  (void)c;
  g.connacks++;
  g.connack_reason = (int)d->reason_code;
}

static void _on_suback(az_mqtt5_client* c, az_mqtt5_suback_data const* d)
{
  (void)c;
  g.subacks++;
  g.suback_count = d->reason_code_count;
  for (int32_t i = 0; i < d->reason_code_count; i++)
  {
    assert_int_equal(d->reason_codes[i], 0);
  }
}

static void _on_publish(az_mqtt5_client* c, az_mqtt5_publish_data const* p)
{
  (void)c;
  (void)p;
  g.publishes++;
}

static void _on_disconnect(az_mqtt5_client* c, az_mqtt5_disconnect_data const* d)
{
  (void)c;
  (void)d;
  g.disconnects++;
}

static void _on_closed(az_mqtt5_client* c, az_result reason)
{
  // The transport is already closed: the client must look disconnected.
  assert_int_equal(az_mqtt5_client_get_state(c), AZ_MQTT5_CLIENT_STATE_DISCONNECTED);
  g.closed++;
  g.closed_reason = reason;
}

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

static void _setup(fixture* f, test_server_options const* so, uint16_t keep_alive_s)
{
  memset(f, 0, sizeof(*f));
  memset(&g, 0, sizeof(g));
  f->server = test_server_start(so);
  assert_non_null(f->server);
  f->transport = (az_mqtt5_transport*)calloc(1, (size_t)az_mqtt5_transport_sizeof());
  assert_non_null(f->transport);
  assert_int_equal(az_mqtt5_transport_init(f->transport), AZ_OK);

  az_mqtt5_client_options o;
  memset(&o, 0, sizeof(o));
  o.transport = f->transport;
  o.send_buffer = ARRAY_SPAN(f->send_buf);
  o.receive_buffer = ARRAY_SPAN(f->recv_buf);
  o.connect_options = az_mqtt5_connect_options_default();
  o.connect_options.client_id = AZ_SPAN_FROM_STR("session-test");
  o.connect_options.keep_alive_seconds = keep_alive_s;
  o.hostname = AZ_SPAN_FROM_STR("127.0.0.1");
  o.port = test_server_port(f->server);
  o.on_connack = _on_connack;
  o.on_publish = _on_publish;
  o.on_suback = _on_suback;
  o.on_disconnect = _on_disconnect;
  o.on_connection_closed = _on_closed;
  o.buffers.connack_user_properties = ARRAY_SPAN(f->props[0]);
  o.buffers.publish_user_properties = ARRAY_SPAN(f->props[1]);
  o.buffers.publish_subscription_identifiers = ARRAY_SPAN(f->sub_ids);
  o.buffers.suback_reason_codes = ARRAY_SPAN(f->reasons);
  o.buffers.suback_user_properties = ARRAY_SPAN(f->props[2]);
  o.buffers.ack_user_properties = ARRAY_SPAN(f->props[3]);
  o.buffers.disconnect_user_properties = ARRAY_SPAN(f->props[3]);
  assert_int_equal(az_mqtt5_client_init(&f->client, &o), AZ_OK);
}

static void _teardown(fixture* f)
{
  (void)az_mqtt5_client_disconnect(&f->client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION);
  free(f->transport);
  test_server_stop(f->server);
}

static test_server_options _plain(void)
{
  test_server_options o = test_server_options_default();
  o.tls = false;
  return o;
}

/** @brief Pump until disconnected or @p budget_ms; returns the last process_loop result. */
static az_result _pump_until_closed(fixture* f, int budget_ms)
{
  az_result rc = AZ_OK;
  int64_t const end = _now_ms() + budget_ms;
  while (az_mqtt5_client_get_state(&f->client) != AZ_MQTT5_CLIENT_STATE_DISCONNECTED
         && _now_ms() < end)
  {
    rc = az_mqtt5_client_process_loop(&f->client, 100);
  }
  return rc;
}

static void a_long_process_loop_wait_still_pings_on_time(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(az_mqtt5_client_connect(&f.client, 3000), AZ_OK);

  // One call asked to wait 5 s must return at the 1 s keep-alive, having pinged.
  int64_t t0 = _now_ms();
  assert_int_equal(az_mqtt5_client_process_loop(&f.client, 5000), AZ_OK);
  int64_t elapsed = _now_ms() - t0;
  assert_true(elapsed >= 900 && elapsed < 2500);
  for (int i = 0; i < 20 && test_server_pingreqs(f.server) == 0; i++)
  {
    (void)az_mqtt5_client_process_loop(&f.client, 50);
  }
  assert_int_equal(test_server_pingreqs(f.server), 1);
  assert_int_equal(az_mqtt5_client_get_state(&f.client), AZ_MQTT5_CLIENT_STATE_CONNECTED);
  _teardown(&f);
}

static void an_idle_session_with_answered_pings_stays_up(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(az_mqtt5_client_connect(&f.client, 3000), AZ_OK);
  int64_t const end = _now_ms() + 3500;
  while (_now_ms() < end)
  {
    assert_int_equal(az_mqtt5_client_process_loop(&f.client, 100), AZ_OK);
  }
  assert_true(test_server_pingreqs(f.server) >= 2);
  assert_int_equal(g.closed, 0);
  _teardown(&f);
}

static void a_missing_pingresp_ends_the_session(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.no_pingresp = true;
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(az_mqtt5_client_connect(&f.client, 3000), AZ_OK);

  int64_t t0 = _now_ms();
  assert_int_equal(_pump_until_closed(&f, 5000), AZ_MQTT5_ERROR_KEEP_ALIVE_TIMEOUT);
  // Ping at 1 s, give up 1 s later.
  assert_true(_now_ms() - t0 < 3000);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT5_ERROR_KEEP_ALIVE_TIMEOUT);
  assert_int_equal(az_mqtt5_client_process_loop(&f.client, 0), AZ_MQTT5_ERROR_NOT_CONNECTED);
  _teardown(&f);
}

static void server_keep_alive_overrides_the_clients(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.server_keep_alive = 1;
  fixture f;
  _setup(&f, &so, 60);
  assert_int_equal(az_mqtt5_client_connect(&f.client, 3000), AZ_OK);
  int64_t const end = _now_ms() + 2500;
  while (_now_ms() < end)
  {
    (void)az_mqtt5_client_process_loop(&f.client, 100);
  }
#if AZ_MQTT5_PROTOCOL_VERSION == 5
  assert_true(test_server_pingreqs(f.server) >= 1);
  assert_int_equal(f.client.keep_alive_seconds, 1);
#else
  // MQTT 3.1.1 has no Server Keep Alive: the client's 60 s stands.
  assert_int_equal(test_server_pingreqs(f.server), 0);
  assert_int_equal(f.client.keep_alive_seconds, 60);
#endif
  _teardown(&f);
}

static void a_server_disconnect_closes_the_connection(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.behavior = TEST_SERVER_DISCONNECT_AFTER_CONNACK;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(az_mqtt5_client_connect(&f.client, 3000), AZ_OK);
  (void)_pump_until_closed(&f, 3000);

  assert_int_equal(az_mqtt5_client_get_state(&f.client), AZ_MQTT5_CLIENT_STATE_DISCONNECTED);
  assert_int_equal(g.closed, 1);
#if AZ_MQTT5_PROTOCOL_VERSION == 5
  assert_int_equal(g.disconnects, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT5_ERROR_SERVER_DISCONNECTED);
#else
  assert_true(az_result_failed(g.closed_reason));
#endif
  _teardown(&f);
}

static void one_process_loop_drains_a_burst(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.burst_publishes = 10;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(az_mqtt5_client_connect(&f.client, 3000), AZ_OK);
  _sleep_ms(200); // Let the burst arrive; one call must then handle all of it.
  assert_int_equal(az_mqtt5_client_process_loop(&f.client, 1000), AZ_OK);
  assert_int_equal(g.publishes, 10);
  _teardown(&f);
}

static void a_local_disconnect_reports_closed_once(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(az_mqtt5_client_connect(&f.client, 3000), AZ_OK);
  assert_int_equal(
      az_mqtt5_client_disconnect(&f.client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION), AZ_OK);
  assert_int_equal(
      az_mqtt5_client_disconnect(&f.client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION), AZ_OK);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_OK);
  for (int i = 0; i < 40 && !test_server_client_closed(f.server); i++)
  {
    _sleep_ms(25);
  }
  assert_true(test_server_client_closed(f.server));
  _teardown(&f);
}

static void connect_to_a_silent_peer_times_out(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.behavior = TEST_SERVER_SILENT;
  fixture f;
  _setup(&f, &so, 30);
  int64_t t0 = _now_ms();
  assert_int_equal(az_mqtt5_client_connect(&f.client, 500), AZ_MQTT5_ERROR_TIMEOUT);
  assert_true(_now_ms() - t0 < 1500);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT5_ERROR_TIMEOUT);
  assert_int_equal(az_mqtt5_client_get_state(&f.client), AZ_MQTT5_CLIENT_STATE_DISCONNECTED);
  _teardown(&f);
}

static void reconnect_after_a_lost_session_works(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.no_pingresp = true;
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(az_mqtt5_client_connect(&f.client, 3000), AZ_OK);
  assert_int_equal(_pump_until_closed(&f, 5000), AZ_MQTT5_ERROR_KEEP_ALIVE_TIMEOUT);
  assert_int_equal(az_mqtt5_client_connect(&f.client, 3000), AZ_OK);
  assert_int_equal(g.connacks, 2);
  _teardown(&f);
}

static void a_refused_connack_code_is_reported_verbatim(void** state)
{
  (void)state;
  test_server_options so = _plain();
  // "Not authorized": MQTT 5 reason 0x87, MQTT 3.1.1 return code 5.
  so.connack_code = AZ_MQTT5_PROTOCOL_VERSION == 5 ? 0x87 : 0x05;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(az_mqtt5_client_connect(&f.client, 3000), AZ_MQTT5_ERROR_NOT_CONNECTED);
  assert_int_equal(g.connacks, 1);
  assert_int_equal(g.connack_reason, so.connack_code);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT5_ERROR_NOT_CONNECTED);
  _teardown(&f);
}

static void suback_reason_codes_never_exceed_the_buffer(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.suback_codes = 10; // The fixture holds 4.
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(az_mqtt5_client_connect(&f.client, 3000), AZ_OK);
  az_mqtt5_subscription sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR("t");
  assert_int_equal(az_mqtt5_client_subscribe(&f.client, &sub, 1, NULL), AZ_OK);
  for (int i = 0; i < 20 && g.subacks == 0; i++)
  {
    assert_int_equal(az_mqtt5_client_process_loop(&f.client, 50), AZ_OK);
  }
  assert_int_equal(g.subacks, 1);
  assert_int_equal(g.suback_count, 4);
  _teardown(&f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_long_process_loop_wait_still_pings_on_time),
    cmocka_unit_test(an_idle_session_with_answered_pings_stays_up),
    cmocka_unit_test(a_missing_pingresp_ends_the_session),
    cmocka_unit_test(server_keep_alive_overrides_the_clients),
    cmocka_unit_test(a_server_disconnect_closes_the_connection),
    cmocka_unit_test(one_process_loop_drains_a_burst),
    cmocka_unit_test(a_local_disconnect_reports_closed_once),
    cmocka_unit_test(connect_to_a_silent_peer_times_out),
    cmocka_unit_test(reconnect_after_a_lost_session_works),
    cmocka_unit_test(a_refused_connack_code_is_reported_verbatim),
    cmocka_unit_test(suback_reason_codes_never_exceed_the_buffer),
  };
  return cmocka_run_group_tests_name("client_session", tests, NULL, NULL);
}
