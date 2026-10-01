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


#include "az_mqtt_test_api.h"

#include "test_server.h"

/** @brief Deliberately discard a result (gcc ignores a (void) cast on warn_unused_result). */
static void _ignore(az_result rc) { (void)rc; }

#define ARRAY_SPAN(a) az_span_create((uint8_t*)(a), (int32_t)sizeof(a))

typedef struct
{
  AZ_MQTT_T(client) client;
  az_mqtt_transport* transport;
  test_server* server;
  uint8_t send_buf[1024];
  uint8_t recv_buf[1024];
  az_mqtt_tls_options tls;
  az_mqtt_inflight_entry inflight_control_buffer[8];
#if AZ_MQTT_TEST_VERSION == 5
  AZ_MQTT_T(user_property) props[4][4];
  int32_t sub_ids[4];
  AZ_MQTT_T(reason_code) reasons[4];
#endif
} fixture;

static struct
{
  int connacks;
  int connack_reason;
  int subacks;
  int32_t suback_count;
  int publishes;
  int pubacks;
  int pubcomps;
  int last_pubcomp_reason;
  int unsubacks;
  int32_t publish_user_properties;
  int32_t publish_subscription_identifiers;
  int disconnects;
  int closed;
  az_result closed_reason;
  int reconnects_left;
  az_result reconnect_rc;
} g;

static void _on_connack(AZ_MQTT_T(client)* c, AZ_MQTT_T(connack_data) const* d)
{
  (void)c;
  g.connacks++;
  g.connack_reason = AZ_MQTT_TEST_CONNACK_CODE(d);
}

static void _on_suback(AZ_MQTT_T(client)* c, AZ_MQTT_T(suback_data) const* d)
{
  (void)c;
  g.subacks++;
#if AZ_MQTT_TEST_VERSION == 5
  g.suback_count = d->reason_code_count;
  for (int32_t i = 0; i < d->reason_code_count; i++)
  {
    assert_int_equal(d->reason_codes[i], 0);
  }
#else
  g.suback_count = az_span_size(d->return_codes);
  for (int32_t i = 0; i < g.suback_count; i++)
  {
    assert_int_equal(az_span_ptr(d->return_codes)[i], AZ_MQTT3_SUBACK_GRANTED_QOS_0);
  }
#endif
}

static void _on_publish(AZ_MQTT_T(client)* c, AZ_MQTT_T(publish_data) const* p)
{
  (void)c;
  g.publishes++;
#if AZ_MQTT_TEST_VERSION == 5
  g.publish_user_properties = p->user_property_count;
  g.publish_subscription_identifiers = p->subscription_identifier_count;
#else
  (void)p;
#endif
}

static void _on_puback(AZ_MQTT_T(client)* c, AZ_MQTT_T(ack_data) const* a)
{
  (void)c;
  (void)a;
  g.pubacks++;
}

static void _on_pubcomp(AZ_MQTT_T(client)* c, AZ_MQTT_T(ack_data) const* a)
{
  (void)c;
  g.pubcomps++;
  g.last_pubcomp_reason = AZ_MQTT_TEST_ACK_REASON(a);
}

#if AZ_MQTT_TEST_VERSION == 5
static void _on_unsuback(AZ_MQTT_T(client)* c, AZ_MQTT_T(suback_data) const* d)
#else
static void _on_unsuback(AZ_MQTT_T(client)* c, AZ_MQTT_T(ack_data) const* d)
#endif
{
  (void)c;
  (void)d;
  g.unsubacks++;
}

#if AZ_MQTT_TEST_VERSION == 5
static void _on_disconnect(AZ_MQTT_T(client)* c, AZ_MQTT_T(disconnect_data) const* d)
{
  (void)c;
  (void)d;
  g.disconnects++;
}
#endif

static void _on_closed(AZ_MQTT_T(client)* c, az_result reason)
{
  // The transport is already closed: the client must look disconnected.
  assert_int_equal(AZ_MQTT_T(client_get_state)(c), AZ_MQTT_CLIENT_STATE_DISCONNECTED);
  g.closed++;
  g.closed_reason = reason;
  if (g.reconnects_left > 0)
  {
    g.reconnects_left--;
    g.reconnect_rc = AZ_MQTT_T(client_connect)(c, 3000);
  }
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

/** @brief In-flight entries the next _setup() gives the client (at most 8); then back to 4. */
static int s_inflight_slots = 4;

/** @brief Next _setup() connects with credentials, a will and a client id that must never be logged. */
static bool s_with_secrets;
static AZ_MQTT_T(will_options) s_will;

static void _setup(fixture* f, test_server_options const* so, uint16_t keep_alive_s)
{
  memset(f, 0, sizeof(*f));
  memset(&g, 0, sizeof(g));
  f->server = test_server_start(so);
  assert_non_null(f->server);
  f->transport = (az_mqtt_transport*)calloc(1, (size_t)az_mqtt_transport_sizeof());
  assert_non_null(f->transport);
  assert_int_equal(az_mqtt_transport_init(f->transport), AZ_OK);

  AZ_MQTT_T(client_options) o;
  memset(&o, 0, sizeof(o));
  o.transport = f->transport;
  o.send_buffer = ARRAY_SPAN(f->send_buf);
  o.receive_buffer = ARRAY_SPAN(f->recv_buf);
  o.connect_options = AZ_MQTT_T(connect_options_default)();
  o.connect_options.client_id = AZ_SPAN_FROM_STR("session-test");
  o.connect_options.keep_alive_seconds = keep_alive_s;
  if (s_with_secrets)
  {
    s_with_secrets = false;
    memset(&s_will, 0, sizeof(s_will));
    s_will.topic = AZ_SPAN_FROM_STR("SECRET-WILL-TOPIC");
    s_will.payload = AZ_SPAN_FROM_STR("SECRET-WILL-PAYLOAD");
    o.connect_options.client_id = AZ_SPAN_FROM_STR("SECRET-CLIENT-ID");
    o.connect_options.username = AZ_SPAN_FROM_STR("SECRET-USERNAME");
    o.connect_options.password = AZ_SPAN_FROM_STR("SECRET-PASSWORD");
    o.connect_options.will = &s_will;
  }
  o.hostname = AZ_SPAN_FROM_STR("127.0.0.1");
  if (so->tls)
  {
    f->tls = az_mqtt_tls_options_default();
    f->tls.ca_cert_path = az_span_create_from_str((char*)(uintptr_t)test_server_ca_path(f->server));
    o.tls_options = &f->tls;
  }
  o.port = test_server_port(f->server);
  o.on_connack = _on_connack;
  o.on_publish = _on_publish;
  o.on_suback = _on_suback;
  o.on_unsuback = _on_unsuback;
  o.on_puback = _on_puback;
  o.on_pubcomp = _on_pubcomp;
  o.inflight_control_buffer = az_span_create(
      (uint8_t*)f->inflight_control_buffer,
      s_inflight_slots * (int32_t)sizeof(az_mqtt_inflight_entry));
  s_inflight_slots = 4; // Reset here: a failed test skips _teardown().
  o.on_connection_closed = _on_closed;
#if AZ_MQTT_TEST_VERSION == 5
  o.on_disconnect = _on_disconnect;
  o.buffers.connack_user_properties = ARRAY_SPAN(f->props[0]);
  o.buffers.publish_user_properties = ARRAY_SPAN(f->props[1]);
  o.buffers.publish_subscription_identifiers = ARRAY_SPAN(f->sub_ids);
  o.buffers.suback_reason_codes = ARRAY_SPAN(f->reasons);
  o.buffers.suback_user_properties = ARRAY_SPAN(f->props[2]);
  o.buffers.ack_user_properties = ARRAY_SPAN(f->props[3]);
  o.buffers.disconnect_user_properties = ARRAY_SPAN(f->props[3]);
#endif
  assert_int_equal(AZ_MQTT_T(client_init)(&f->client, &o), AZ_OK);
}

static void _teardown(fixture* f)
{
  _ignore(AZ_MQTT_TEST_DISCONNECT(&f->client));
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
  while (AZ_MQTT_T(client_get_state)(&f->client) != AZ_MQTT_CLIENT_STATE_DISCONNECTED
         && _now_ms() < end)
  {
    rc = AZ_MQTT_T(client_process_loop)(&f->client, 100);
  }
  return rc;
}

static void a_long_process_loop_wait_still_pings_on_time(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);

  // One call asked to wait 5 s must return at the 1 s keep-alive, having pinged.
  int64_t t0 = _now_ms();
  assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 5000), AZ_OK);
  int64_t elapsed = _now_ms() - t0;
  assert_true(elapsed >= 900 && elapsed < 2500);
  for (int i = 0; i < 20 && test_server_pingreqs(f.server) == 0; i++)
  {
    _ignore(AZ_MQTT_T(client_process_loop)(&f.client, 50));
  }
  assert_int_equal(test_server_pingreqs(f.server), 1);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  _teardown(&f);
}

static void an_idle_session_with_answered_pings_stays_up(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  int64_t const end = _now_ms() + 3500;
  while (_now_ms() < end)
  {
    assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 100), AZ_OK);
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
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);

  int64_t t0 = _now_ms();
  assert_int_equal(_pump_until_closed(&f, 5000), AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT);
  // Ping at 1 s, give up 1 s later.
  assert_true(_now_ms() - t0 < 3000);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT);
  assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 0), AZ_MQTT_ERROR_NOT_CONNECTED);
  _teardown(&f);
}

static void server_keep_alive_overrides_the_clients(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.server_keep_alive = 1;
  fixture f;
  _setup(&f, &so, 60);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  int64_t const end = _now_ms() + 2500;
  while (_now_ms() < end)
  {
    _ignore(AZ_MQTT_T(client_process_loop)(&f.client, 100));
  }
#if AZ_MQTT_TEST_VERSION == 5
  assert_true(test_server_pingreqs(f.server) >= 1);
  assert_int_equal(f.client._internal.core._internal.keep_alive_seconds, 1);
#else
  // MQTT 3.1.1 has no Server Keep Alive: the client's 60 s stands.
  assert_int_equal(test_server_pingreqs(f.server), 0);
  assert_int_equal(f.client._internal.core._internal.keep_alive_seconds, 60);
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
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  (void)_pump_until_closed(&f, 3000);

  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_DISCONNECTED);
  assert_int_equal(g.closed, 1);
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(g.disconnects, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_SERVER_DISCONNECTED);
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
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  _sleep_ms(200); // Let the burst arrive; one call must then handle all of it.
  assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 1000), AZ_OK);
  assert_int_equal(g.publishes, 10);
  _teardown(&f);
}

static void a_local_disconnect_reports_closed_once(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(
      AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  assert_int_equal(
      AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
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
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 500), AZ_MQTT_ERROR_TIMEOUT);
  assert_true(_now_ms() - t0 < 1500);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_TIMEOUT);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_DISCONNECTED);
  _teardown(&f);
}

/** @brief Pump connect_start's connect with process_loop(0); each call must return promptly. */
static az_result _pump_connect(fixture* f, int budget_ms)
{
  az_result rc = AZ_OK;
  int64_t const end = _now_ms() + budget_ms;
  while (az_result_succeeded(rc)
         && AZ_MQTT_T(client_get_state)(&f->client) == AZ_MQTT_CLIENT_STATE_CONNECTING
         && _now_ms() < end)
  {
    int64_t const t0 = _now_ms();
    rc = AZ_MQTT_T(client_process_loop)(&f->client, 0);
    assert_true(_now_ms() - t0 < 200);
    _sleep_ms(5);
  }
  return rc;
}

static void a_started_connect_completes_in_process_loop(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect_start)(&f.client, 3000), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTING);
  assert_int_equal(AZ_MQTT_T(client_connect_start)(&f.client, 3000), AZ_MQTT_ERROR_INVALID_STATE);
  assert_int_equal(_pump_connect(&f, 3000), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  assert_int_equal(g.connacks, 1);
  assert_int_equal(g.closed, 0);
  _teardown(&f);
}

#if !defined(AZ_MQTT_TEST_BACKEND_NONE)
static void a_started_tls_connect_resumes_across_calls(void** state)
{
  (void)state;
  test_server_options so = test_server_options_default(); // TLS
  so.handshake_delay_ms = 500;
  fixture f;
  _setup(&f, &so, 30);
  int64_t const start = _now_ms();
  assert_int_equal(AZ_MQTT_T(client_connect_start)(&f.client, 5000), AZ_OK);
  assert_true(_now_ms() - start < 200);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTING);
  // The peer holds the handshake: these calls return at once, still connecting.
  for (int i = 0; i < 3; i++)
  {
    int64_t const t0 = _now_ms();
    assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 0), AZ_OK);
    assert_true(_now_ms() - t0 < 200);
    assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTING);
  }
  assert_int_equal(_pump_connect(&f, 5000), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  assert_int_equal(test_server_handshakes(f.server), 1);
  assert_int_equal(g.connacks, 1);
  _teardown(&f);
}
#endif

static void a_started_connect_to_a_silent_peer_times_out(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.behavior = TEST_SERVER_SILENT;
  fixture f;
  _setup(&f, &so, 30);
  int64_t const start = _now_ms();
  assert_int_equal(AZ_MQTT_T(client_connect_start)(&f.client, 500), AZ_OK);
  assert_int_equal(_pump_connect(&f, 5000), AZ_MQTT_ERROR_TIMEOUT);
  int64_t const elapsed = _now_ms() - start;
  assert_true(elapsed >= 450 && elapsed < 2000);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_DISCONNECTED);
  assert_int_equal(g.connacks, 0);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_TIMEOUT);
  _teardown(&f);
}

static void a_started_connect_reports_a_refused_connack(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.connack_code = AZ_MQTT_TEST_VERSION == 5 ? 0x87 : 0x05;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect_start)(&f.client, 3000), AZ_OK);
  assert_int_equal(_pump_connect(&f, 3000), AZ_MQTT_ERROR_NOT_CONNECTED);
  assert_int_equal(g.connacks, 1);
  assert_int_equal(g.connack_reason, so.connack_code);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_NOT_CONNECTED);
  _teardown(&f);
}

static void disconnect_cancels_a_started_connect(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.behavior = TEST_SERVER_SILENT;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect_start)(&f.client, 5000), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 0), AZ_OK);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_DISCONNECTED);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 0), AZ_MQTT_ERROR_NOT_CONNECTED);
  _teardown(&f);
}

/** @brief Run process_loop until @p cond holds (3 s at most). */
#define PUMP_UNTIL(fx, cond)                                                                    \
  do                                                                                            \
  {                                                                                             \
    int64_t const end_ = _now_ms() + 3000;                                                      \
    while (!(cond) && _now_ms() < end_)                                                         \
    {                                                                                           \
      assert_int_equal(AZ_MQTT_T(client_process_loop)(&(fx)->client, 20), AZ_OK);              \
    }                                                                                           \
  } while (0)

static AZ_MQTT_T(publish_options) _publish_options(az_mqtt_qos qos)
{
  AZ_MQTT_T(publish_options) p = AZ_MQTT_T(publish_options_default)();
  p.topic = AZ_SPAN_FROM_STR("t");
  p.payload = AZ_SPAN_FROM_STR("p");
  p.qos = qos;
  return p;
}

static az_result _subscribe(fixture* f)
{
  AZ_MQTT_T(subscription) sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR("t");
  return AZ_MQTT_T(client_subscribe)(&f->client, &sub, 1, NULL);
}

static void inflight_slots_bound_requests(void** state)
{
  (void)state;
  test_server_options so = _plain(); // Never acknowledges a PUBLISH.
  s_inflight_slots = 3;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) const qos1 = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  uint16_t ids[3];
  for (int i = 0; i < 3; i++)
  {
    assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, &ids[i]), AZ_OK);
    assert_int_not_equal(ids[i], 0);
    for (int j = 0; j < i; j++)
    {
      assert_int_not_equal(ids[i], ids[j]);
    }
  }
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, NULL), AZ_MQTT_ERROR_FLOW_CONTROL);
  assert_int_equal(_subscribe(&f), AZ_MQTT_ERROR_FLOW_CONTROL);
  AZ_MQTT_T(publish_options) const qos0 = _publish_options(AZ_MQTT_QOS_AT_MOST_ONCE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos0, NULL), AZ_OK);
  PUMP_UNTIL(&f, test_server_publishes(f.server) == 4);
  assert_int_equal(test_server_publishes(f.server), 4); // Refused requests were never sent.
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  _teardown(&f);
}

static void acknowledged_requests_free_their_slots(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.ack_publishes = true;
  s_inflight_slots = 1;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) const qos1 = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  AZ_MQTT_T(publish_options) const qos2 = _publish_options(AZ_MQTT_QOS_EXACTLY_ONCE);
  for (int i = 1; i <= 3; i++)
  {
    assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, NULL), AZ_OK);
    PUMP_UNTIL(&f, g.pubacks == i);
    assert_int_equal(g.pubacks, i);
  }
  for (int i = 1; i <= 3; i++)
  {
    assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos2, NULL), AZ_OK);
    PUMP_UNTIL(&f, g.pubcomps == i);
    assert_int_equal(g.pubcomps, i);
  }
  assert_int_equal(test_server_pubrels(f.server), 3);
  assert_int_equal(_subscribe(&f), AZ_OK);
  PUMP_UNTIL(&f, g.subacks == 1);
  az_span const filter = AZ_SPAN_FROM_STR("t");
  assert_int_equal(AZ_MQTT_T(client_unsubscribe)(&f.client, &filter, 1, NULL), AZ_OK);
  PUMP_UNTIL(&f, g.unsubacks == 1);
  assert_int_equal(g.subacks, 1);
  assert_int_equal(g.unsubacks, 1);
  _teardown(&f);
}

static void a_held_packet_identifier_is_not_reused(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.ack_publishes = true;
  so.hold_first_publish = true;
  s_inflight_slots = 2;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) const qos1 = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  uint16_t held;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, &held), AZ_OK);
  for (int i = 1; i <= 65535; i++) // Every identifier comes round once.
  {
    uint16_t id;
    assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, &id), AZ_OK);
    assert_int_not_equal(id, held);
    PUMP_UNTIL(&f, g.pubacks == i);
    assert_int_equal(g.pubacks, i);
  }
  _teardown(&f);
}

static void a_session_end_abandons_what_is_in_flight(void** state)
{
  (void)state;
  test_server_options so = _plain();
  s_inflight_slots = 1;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) const qos1 = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, NULL), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, NULL), AZ_MQTT_ERROR_FLOW_CONTROL);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, NULL), AZ_OK);
  _teardown(&f);
}

static void inbound_qos2_duplicates_are_delivered_once(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.send_qos2_sequence = true; // 7, 7 (DUP), PUBREL 7, 7, PUBREL 7
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  PUMP_UNTIL(&f, test_server_pubcomps(f.server) == 2);
  assert_int_equal(test_server_pubcomps(f.server), 2);
  assert_int_equal(g.publishes, 2);
  assert_int_equal(g.pubcomps, 2); // Inbound exchanges completed.
  assert_int_equal(test_server_last_pubcomp_reason(f.server), 0);
  _teardown(&f);
}

static void inbound_qos2_without_a_free_slot_is_still_delivered(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.send_qos2_sequence = true;
  s_inflight_slots = 0;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  PUMP_UNTIL(&f, test_server_pubcomps(f.server) == 2);
  assert_int_equal(test_server_pubcomps(f.server), 2); // Every PUBREL still answered.
  assert_int_equal(g.publishes, 3);                    // No duplicate detection.
  assert_int_equal(g.pubcomps, 0);                     // Untracked: not reported.
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(test_server_last_pubcomp_reason(f.server), 0x92); // Packet Identifier not found
#endif
  _teardown(&f);
}

static void unknown_acknowledgements_are_ignored(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.send_unknown_acks = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  PUMP_UNTIL(&f, test_server_pubrels(f.server) == 1 && test_server_pubcomps(f.server) == 1);
  assert_int_equal(test_server_pubrels(f.server), 1);  // Answers the PUBREC,
  assert_int_equal(test_server_pubcomps(f.server), 1); // and the PUBREL.
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(test_server_last_pubrel_reason(f.server), 0x92); // Packet Identifier not found
  assert_int_equal(test_server_last_pubcomp_reason(f.server), 0x92);
#endif
  assert_int_equal(g.pubacks, 0);
  assert_int_equal(g.pubcomps, 0);
  assert_int_equal(g.subacks, 0);
  assert_int_equal(g.unsubacks, 0);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  _teardown(&f);
}

#if AZ_MQTT_TEST_VERSION == 5
static void the_server_receive_maximum_limits_publishes(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.receive_maximum = 2;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) const qos1 = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  AZ_MQTT_T(publish_options) const qos2 = _publish_options(AZ_MQTT_QOS_EXACTLY_ONCE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, NULL), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos2, NULL), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, NULL), AZ_MQTT_ERROR_FLOW_CONTROL);
  assert_int_equal(_subscribe(&f), AZ_OK); // Not a PUBLISH: not limited by Receive Maximum.
  _teardown(&f);
}

static void the_server_limits_are_enforced(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.maximum_qos_present = true;
  so.maximum_qos = 1;
  so.retain_unavailable = true;
  so.maximum_packet_size = 64;
  s_inflight_slots = 1;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) p = _publish_options(AZ_MQTT_QOS_EXACTLY_ONCE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_MQTT_ERROR_NOT_SUPPORTED);
  p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  p.retain = true;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_MQTT_ERROR_NOT_SUPPORTED);
  p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  p.topic_alias = 1; // The server sent no Topic Alias Maximum: 0.
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_MQTT_ERROR_NOT_SUPPORTED);
  p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  p.payload = AZ_SPAN_FROM_STR("0123456789012345678901234567890123456789012345678901234567890123");
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_MQTT_ERROR_PACKET_TOO_LARGE);
  p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE); // The refused one freed its slot.
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_OK);
  PUMP_UNTIL(&f, test_server_publishes(f.server) == 1);
  _sleep_ms(50);
  assert_int_equal(test_server_publishes(f.server), 1);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  _teardown(&f);
}

static void an_acknowledgement_over_the_server_maximum_packet_size_closes(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.maximum_packet_size = 3; // A PUBREC is 4 bytes.
  so.send_qos2_sequence = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  int64_t const end = _now_ms() + 3000;
  az_result rc = AZ_OK;
  while (rc == AZ_OK && _now_ms() < end)
  {
    rc = AZ_MQTT_T(client_process_loop)(&f.client, 20);
  }
  assert_int_equal(rc, AZ_MQTT_ERROR_PACKET_TOO_LARGE);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_PACKET_TOO_LARGE);
  assert_int_equal(g.publishes, 0); // Not delivered: it could not be acknowledged.
  _teardown(&f);
}

static void a_failed_pubrec_ends_the_exchange(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.ack_publishes = true;
  so.pubrec_reason = 0x80;
  s_inflight_slots = 1;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) const qos2 = _publish_options(AZ_MQTT_QOS_EXACTLY_ONCE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos2, NULL), AZ_OK);
  PUMP_UNTIL(&f, g.pubcomps == 1);
  assert_int_equal(g.pubcomps, 1);
  assert_int_equal(g.last_pubcomp_reason, 0x80);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos2, NULL), AZ_OK); // Slot freed.
  PUMP_UNTIL(&f, g.pubcomps == 2);
  assert_int_equal(test_server_pubrels(f.server), 0);
  _teardown(&f);
}
#endif

static void a_peer_close_is_reported_as_connection_closed(void** state)
{
  (void)state;
#if defined(AZ_MQTT_TEST_BACKEND_NONE)
  int const tls_cases = 1;
#else
  int const tls_cases = 2;
#endif
  for (int tls = 0; tls < tls_cases; tls++)
  {
    test_server_options so = _plain();
    so.tls = tls == 1;
    so.close_after_connack = true;
    fixture f;
    _setup(&f, &so, 30);
    az_result const rc = AZ_MQTT_T(client_connect)(&f.client, 3000);
    // The close may be read in the same loop as the CONNACK.
    assert_true(rc == AZ_OK || rc == AZ_MQTT_ERROR_CONNECTION_CLOSED);
    _ignore(_pump_until_closed(&f, 3000));
    assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_CONNECTION_CLOSED);
    _teardown(&f);
  }
}

#ifndef AZ_NO_LOGGING
/** @brief Every az_log message, each followed by '\n'. */
static char s_log[16384];
static size_t s_log_size;

static void _on_log(az_log_classification classification, az_span message)
{
  (void)classification;
  size_t const size = (size_t)az_span_size(message);
  if (s_log_size + size + 2 <= sizeof(s_log))
  {
    memcpy(s_log + s_log_size, az_span_ptr(message), size);
    s_log_size += size;
    s_log[s_log_size++] = '\n';
    s_log[s_log_size] = '\0';
  }
}

static void logs_never_contain_credentials_topics_or_payloads(void** state)
{
  (void)state;
  s_log_size = 0;
  s_log[0] = '\0';
  az_log_set_message_callback(_on_log);
  test_server_options so = test_server_options_default(); // TLS
#if defined(AZ_MQTT_TEST_BACKEND_NONE)
  so.tls = false;
#endif
  so.ack_publishes = true;
  s_with_secrets = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(subscription) sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR("SECRET-FILTER");
  assert_int_equal(AZ_MQTT_T(client_subscribe)(&f.client, &sub, 1, NULL), AZ_OK);
  AZ_MQTT_T(publish_options) p = AZ_MQTT_T(publish_options_default)();
  p.topic = AZ_SPAN_FROM_STR("SECRET-TOPIC");
  p.payload = AZ_SPAN_FROM_STR("SECRET-PAYLOAD");
  p.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_OK);
  int64_t const end = _now_ms() + 3000;
  while ((g.subacks == 0 || g.pubacks == 0) && _now_ms() < end)
  {
    assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 20), AZ_OK);
  }
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  az_log_set_message_callback(NULL);
  _teardown(&f);

  assert_non_null(strstr(s_log, "connect 127.0.0.1:"));
#if !defined(AZ_MQTT_TEST_BACKEND_NONE)
  assert_non_null(strstr(s_log, " tls\n"));
#endif
  assert_non_null(strstr(s_log, "sent CONNECT "));
  assert_non_null(strstr(s_log, "received CONNACK "));
  assert_non_null(strstr(s_log, "sent SUBSCRIBE "));
  assert_non_null(strstr(s_log, "received PUBACK "));
  assert_non_null(strstr(s_log, "closed 0x00010000 native 0:0\n"));
  assert_null(strstr(s_log, "SECRET"));
}
#endif // AZ_NO_LOGGING

static void reconnect_after_a_lost_session_works(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.no_pingresp = true;
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(_pump_until_closed(&f, 5000), AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(g.connacks, 2);
  _teardown(&f);
}

static void a_refused_connack_code_is_reported_verbatim(void** state)
{
  (void)state;
  test_server_options so = _plain();
  // "Not authorized": MQTT 5 reason 0x87, MQTT 3.1.1 return code 5.
  so.connack_code = AZ_MQTT_TEST_VERSION == 5 ? 0x87 : 0x05;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_MQTT_ERROR_NOT_CONNECTED);
  assert_int_equal(g.connacks, 1);
  assert_int_equal(g.connack_reason, so.connack_code);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_NOT_CONNECTED);
  _teardown(&f);
}

static void suback_reason_codes_never_exceed_the_buffer(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.suback_codes = 10; // The fixture holds 4.
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(subscription) sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR("t");
  assert_int_equal(AZ_MQTT_T(client_subscribe)(&f.client, &sub, 1, NULL), AZ_OK);
  for (int i = 0; i < 20 && g.subacks == 0; i++)
  {
    assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 50), AZ_OK);
  }
  assert_int_equal(g.subacks, 1);
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(g.suback_count, 4);
#else
  assert_int_equal(g.suback_count, 10); // A view of the packet: no buffer to overflow.
#endif
  _teardown(&f);
}

#if AZ_MQTT_TEST_VERSION == 5
static void publish_properties_never_exceed_the_buffers(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.publish_properties = 10; // The fixture holds 4 of each.
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  for (int i = 0; i < 20 && g.publishes == 0; i++)
  {
    assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 50), AZ_OK);
  }
  assert_int_equal(g.publishes, 1);
  assert_int_equal(g.publish_user_properties, 4);
  assert_int_equal(g.publish_subscription_identifiers, 4);
  assert_int_equal(f.sub_ids[3], 4);
  // The arrays that follow the publish buffers are untouched.
  assert_null(az_span_ptr(f.props[2][0].key));
  assert_int_equal(f.reasons[0], 0);
  _teardown(&f);
}
#endif

static void an_auth_packet_is_a_protocol_error_only_in_mqttv3(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.send_auth = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  az_result rc = AZ_OK;
  for (int i = 0; i < 10 && rc == AZ_OK; i++)
  {
    rc = AZ_MQTT_T(client_process_loop)(&f.client, 50);
  }
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
#else
  assert_int_equal(rc, AZ_MQTT_ERROR_PROTOCOL);
#endif
  _teardown(&f);
}

static void a_server_disconnect_is_a_protocol_error_only_in_mqttv3(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.send_empty_disconnect = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  (void)_pump_until_closed(&f, 3000);
  assert_int_equal(g.closed, 1);
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_SERVER_DISCONNECTED);
#else
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_PROTOCOL);
#endif
  _teardown(&f);
}

static void an_explicit_server_keep_alive_of_zero_disables_pings(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.server_keep_alive = 0;
  so.server_keep_alive_present = true;
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  int64_t const end = _now_ms() + 2500;
  while (_now_ms() < end)
  {
    assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 100), AZ_OK);
  }
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(f.client._internal.core._internal.keep_alive_seconds, 0);
  assert_int_equal(test_server_pingreqs(f.server), 0);
#else
  // No such property in MQTT 3.1.1: the client's 1 s stands.
  assert_int_equal(f.client._internal.core._internal.keep_alive_seconds, 1);
  assert_true(test_server_pingreqs(f.server) >= 1);
#endif
  _teardown(&f);
}

static void reconnecting_from_on_connection_closed_is_safe(void** state)
{
  (void)state;
  // Each session gets CONNACK then DISCONNECT (MQTT 5) or a close (3.1.1).
  test_server_options so = _plain();
  so.behavior = TEST_SERVER_DISCONNECT_AFTER_CONNACK;
  fixture f;
  _setup(&f, &so, 30);
  g.reconnects_left = 1;
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);

  // First end: the callback reconnects synchronously.
  for (int i = 0; i < 30 && g.closed == 0; i++)
  {
    _ignore(AZ_MQTT_T(client_process_loop)(&f.client, 100));
  }
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.reconnect_rc, AZ_OK);
  assert_int_equal(g.connacks, 2);

  // The new session must still see its own end, intact.
  (void)_pump_until_closed(&f, 3000);
  assert_int_equal(g.closed, 2);
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(g.disconnects, 2);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_SERVER_DISCONNECTED);
#endif
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
    cmocka_unit_test(a_started_connect_completes_in_process_loop),
#if !defined(AZ_MQTT_TEST_BACKEND_NONE)
    cmocka_unit_test(a_started_tls_connect_resumes_across_calls),
#endif
    cmocka_unit_test(a_started_connect_to_a_silent_peer_times_out),
    cmocka_unit_test(a_started_connect_reports_a_refused_connack),
    cmocka_unit_test(disconnect_cancels_a_started_connect),
    cmocka_unit_test(inflight_slots_bound_requests),
    cmocka_unit_test(acknowledged_requests_free_their_slots),
    cmocka_unit_test(a_held_packet_identifier_is_not_reused),
    cmocka_unit_test(a_session_end_abandons_what_is_in_flight),
    cmocka_unit_test(inbound_qos2_duplicates_are_delivered_once),
    cmocka_unit_test(inbound_qos2_without_a_free_slot_is_still_delivered),
    cmocka_unit_test(unknown_acknowledgements_are_ignored),
#if AZ_MQTT_TEST_VERSION == 5
    cmocka_unit_test(the_server_receive_maximum_limits_publishes),
    cmocka_unit_test(the_server_limits_are_enforced),
    cmocka_unit_test(an_acknowledgement_over_the_server_maximum_packet_size_closes),
    cmocka_unit_test(a_failed_pubrec_ends_the_exchange),
#endif
    cmocka_unit_test(a_peer_close_is_reported_as_connection_closed),
#ifndef AZ_NO_LOGGING
    cmocka_unit_test(logs_never_contain_credentials_topics_or_payloads),
#endif
    cmocka_unit_test(reconnect_after_a_lost_session_works),
    cmocka_unit_test(a_refused_connack_code_is_reported_verbatim),
    cmocka_unit_test(suback_reason_codes_never_exceed_the_buffer),
#if AZ_MQTT_TEST_VERSION == 5
    cmocka_unit_test(publish_properties_never_exceed_the_buffers),
#endif
    cmocka_unit_test(an_auth_packet_is_a_protocol_error_only_in_mqttv3),
    cmocka_unit_test(a_server_disconnect_is_a_protocol_error_only_in_mqttv3),
    cmocka_unit_test(an_explicit_server_keep_alive_of_zero_disables_pings),
    cmocka_unit_test(reconnecting_from_on_connection_closed_is_safe),
  };
  return cmocka_run_group_tests_name("client_session", tests, NULL, NULL);
}
