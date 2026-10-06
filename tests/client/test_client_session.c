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
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>


#include "az_mqtt_test_api.h"

#include <az_mqtt/az_mqtt_websocket.h>

#include "test_native_errors.h"
#include "test_proxy.h"
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
  uint8_t inflight_message_buffer[4096];
  az_mqtt_websocket ws;
  az_span ws_path;
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
  int incoming_pubcomps;
  int last_pubcomp_reason;
  int unsubacks;
  int32_t publish_user_properties;
  int32_t publish_subscription_identifiers;
  int disconnects;
  int closed;
  az_result closed_reason;
  /** @brief Native transport errors, and how many had arrived when the session closed. */
  test_native_errors native;
  int native_at_close;
  AZ_MQTT_T(client) const* native_client;
  int reconnects_left;
  az_result reconnect_rc;
  /** @brief Outgoing exchanges dropped unacknowledged: count, statuses, packet ids, QoS 1 ones. */
  int drops;
  az_result drop_status[8];
  uint16_t drop_ids[8];
  int drop_qos1;
  int last_drop_reason;
  /** @brief Disconnect at the first drop. */
  bool disconnect_on_drop;
  /** @brief Publish a QoS 1 message at each drop; the results and packet ids. */
  bool publish_on_drop;
  az_result drop_publish_rc[8];
  uint16_t drop_publish_ids[8];
  /** @brief on_connack publishes a QoS 1 message (the topic "n"). */
  bool publish_on_connack;
  az_result connack_publish_rc;
} g;

static void _on_connack(AZ_MQTT_T(client)* c, AZ_MQTT_T(connack_data) const* d)
{
  g.connacks++;
  g.connack_reason = AZ_MQTT_TEST_CONNACK_CODE(d);
  if (g.publish_on_connack)
  {
    g.publish_on_connack = false;
    AZ_MQTT_T(publish_options) p = AZ_MQTT_T(publish_options_default)();
    p.topic = AZ_SPAN_FROM_STR("n");
    p.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
    g.connack_publish_rc = AZ_MQTT_T(client_publish)(c, &p, NULL);
  }
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

/** @brief Record an exchange dropped unacknowledged. */
static void _record_drop(AZ_MQTT_T(client)* c, AZ_MQTT_T(ack_data) const* a, bool qos1)
{
  int const n = g.drops < 8 ? g.drops : 7;
  g.drops++;
  g.drop_status[n] = a->status;
  g.drop_ids[n] = a->packet_id;
  g.drop_qos1 += qos1;
  g.last_drop_reason = AZ_MQTT_TEST_ACK_REASON(a);
  if (g.disconnect_on_drop)
  {
    g.disconnect_on_drop = false;
    _ignore(AZ_MQTT_TEST_DISCONNECT(c));
  }
  if (g.publish_on_drop)
  {
    AZ_MQTT_T(publish_options) p = AZ_MQTT_T(publish_options_default)();
    p.topic = AZ_SPAN_FROM_STR("n");
    p.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
    g.drop_publish_rc[n] = AZ_MQTT_T(client_publish)(c, &p, &g.drop_publish_ids[n]);
  }
}

static void _on_puback(AZ_MQTT_T(client)* c, AZ_MQTT_T(ack_data) const* a)
{
  if (a->status != AZ_OK)
  {
    _record_drop(c, a, true);
    return;
  }
  g.pubacks++;
}

static void _on_pubcomp(AZ_MQTT_T(client)* c, AZ_MQTT_T(ack_data) const* a)
{
  if (a->status != AZ_OK)
  {
    _record_drop(c, a, false);
    return;
  }
  g.pubcomps++;
  g.incoming_pubcomps += a->incoming ? 1 : 0;
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

static void _on_transport_error(AZ_MQTT_T(client)* c, az_mqtt_native_error const* error)
{
  g.native_client = c;
  test_native_errors_record(error, &g.native);
}

/** @brief Next _setup() keeps the session (Clean Session / Clean Start 0; then reset). */
static bool s_persistent;

/** @brief inflight_message_buffer bytes the next _setup() gives (then back to all 4096). */
static int s_message_storage = 4096;

/** @brief Next _setup() records the init result in s_init_rc instead of asserting success. */
static bool s_init_may_fail;
static az_result s_init_rc;

static void _on_closed(AZ_MQTT_T(client)* c, az_result reason)
{
  // The transport is already closed: the client must look disconnected.
  assert_int_equal(AZ_MQTT_T(client_get_state)(c), AZ_MQTT_CLIENT_STATE_DISCONNECTED);
  g.closed++;
  g.closed_reason = reason;
  g.native_at_close = g.native.count;
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
#if AZ_MQTT_TEST_VERSION == 5
/** @brief Receive Maximum the next _setup() advertises (0: the default). */
static uint16_t s_receive_maximum;
#endif

/** @brief Next _setup() connects with credentials, a will and a client id that must never be logged. */
static bool s_with_secrets;
static AZ_MQTT_T(will_options) s_will;

/** @brief Proxy the next _setup() connects through (then reset); NULL: none. */
static az_mqtt_proxy_options const* s_proxy;

static void _setup(fixture* f, test_server_options const* server_options, uint16_t keep_alive_s)
{
  memset(f, 0, sizeof(*f));
  memset(&g, 0, sizeof(g));
  test_server_options so_copy = *server_options;
#ifdef AZ_MQTT_TEST_FORCE_WEBSOCKET
  so_copy.websocket = true; // The whole suite, over WebSockets.
#endif
  test_server_options const* const so = &so_copy;
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
    f->ws_path = AZ_SPAN_FROM_STR("/SECRET-PATH");
  }
  if (so->websocket)
  {
    az_mqtt_websocket_options ws_options = az_mqtt_websocket_options_default();
    ws_options.path = f->ws_path;
    assert_int_equal(az_mqtt_websocket_init(&f->ws, f->transport, &ws_options), AZ_OK);
    o.transport = az_mqtt_websocket_get_transport(&f->ws);
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
#if AZ_MQTT_TEST_VERSION == 5
  if (s_receive_maximum != 0)
  {
    o.connect_options.receive_maximum = s_receive_maximum;
    s_receive_maximum = 0;
  }
#endif
  o.on_connection_closed = _on_closed;
  o.proxy_options = s_proxy;
  s_proxy = NULL;
  o.on_transport_error = _on_transport_error;
  o.inflight_message_buffer = az_span_create(f->inflight_message_buffer, s_message_storage);
  s_message_storage = (int)sizeof(f->inflight_message_buffer);
  if (s_persistent)
  {
    s_persistent = false;
#if AZ_MQTT_TEST_VERSION == 5
    o.connect_options.clean_start = false;
    o.connect_options.session_expiry_interval = 3600;
#else
    o.connect_options.clean_session = false;
#endif
  }
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
  az_result const init_rc = AZ_MQTT_T(client_init)(&f->client, &o);
  if (s_init_may_fail)
  {
    s_init_may_fail = false;
    s_init_rc = init_rc;
    return;
  }
  assert_int_equal(init_rc, AZ_OK);
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

#if !defined(AZ_MQTT_TEST_BACKEND_NONE)
static void data_after_a_session_ticket_is_read_without_waiting(void** state)
{
  (void)state;
  test_server_options so = test_server_options_default(); // TLS 1.3
  so.burst_publishes = 10;
  so.ticket_before_burst = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  _sleep_ms(200); // Ticket and burst have arrived.
  if (!test_server_last_tls13(f.server))
  {
    _teardown(&f); // No TLS 1.3 in this build: no post-handshake ticket to test.
    skip();
  }
  assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 0), AZ_OK);
  assert_int_equal(g.publishes, 10);
  _teardown(&f);
}
#endif

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
  assert_int_equal(g.incoming_pubcomps, 0); // All outgoing.
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

/**
 * @brief Connect to a server that answers only QoS 2 with PUBREC; leave QoS 1 (a), QoS 2 at
 * PUBREL (b) and QoS 1 (c) unacknowledged, then disconnect. The server log is cleared.
 */
static void _leave_three_in_flight(fixture* f, uint16_t ids[3])
{
  assert_int_equal(AZ_MQTT_T(client_connect)(&f->client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) const qos1 = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  AZ_MQTT_T(publish_options) const qos2 = _publish_options(AZ_MQTT_QOS_EXACTLY_ONCE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f->client, &qos1, &ids[0]), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f->client, &qos2, &ids[1]), AZ_OK);
  PUMP_UNTIL(f, test_server_pubrels(f->server) == 1);
  assert_int_equal(test_server_pubrels(f->server), 1);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f->client, &qos1, &ids[2]), AZ_OK);
  PUMP_UNTIL(f, test_server_publishes(f->server) == 3);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f->client), AZ_OK);
  char log[256];
  test_server_take_log(f->server, log, (int)sizeof(log));
}

/** @brief The server log, once it holds @p count entries (or after 3 s). */
static void _take_log(fixture* f, int count, char* out, int size)
{
  int64_t const end = _now_ms() + 3000;
  char log[256] = "";
  for (;;)
  {
    size_t const used = strlen(log);
    test_server_take_log(f->server, log + used, (int)(sizeof(log) - used));
    int entries = 0;
    for (char const* p = log; *p != '\0'; p++)
    {
      entries += *p == ' ';
    }
    if (entries >= count || _now_ms() >= end)
    {
      break;
    }
    _ignore(AZ_MQTT_T(client_process_loop)(&f->client, 20));
  }
  _sleep_ms(50); // Nothing more.
  size_t const used = strlen(log);
  test_server_take_log(f->server, log + used, (int)(sizeof(log) - used));
  (void)snprintf(out, (size_t)size, "%s", log);
}

/** @brief _setup() of a kept session with a PUBREC-only server; three left in flight. */
static void _setup_resume(fixture* f, uint16_t ids[3], int slots)
{
  test_server_options so = _plain();
  so.pubrec_only = true;
  s_inflight_slots = slots;
  s_persistent = true;
  _setup(f, &so, 30);
  _leave_three_in_flight(f, ids);
  test_server_set_session_present(f->server, true);
}

static void a_resumed_session_resends_pubrel_then_publishes_oldest_first(void** state)
{
  (void)state;
  fixture f;
  uint16_t ids[3];
  _setup_resume(&f, ids, 4);
  g.publish_on_connack = true; // A new one, after the resends.
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(g.connack_publish_rc, AZ_OK);
  char expected[64];
  (void)snprintf(
      expected, sizeof(expected), "R:%u P1:%ud P1:%ud P1:%u ", ids[1], ids[0], ids[2], ids[2] + 1);
  char log[256];
  _take_log(&f, 4, log, (int)sizeof(log));
  assert_string_equal(log, expected);
  assert_int_equal(g.drops, 0);

  // Still unacknowledged: resent again on the next resume.
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  (void)snprintf(
      expected, sizeof(expected), "R:%u P1:%ud P1:%ud P1:%ud ", ids[1], ids[0], ids[2], ids[2] + 1);
  _take_log(&f, 4, log, (int)sizeof(log));
  assert_string_equal(log, expected);
  _teardown(&f);
}

static void without_session_present_each_exchange_is_reported_dropped(void** state)
{
  (void)state;
  fixture f;
  uint16_t ids[3];
  _setup_resume(&f, ids, 3);
  test_server_set_session_present(f.server, false);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(g.drops, 3);
  assert_int_equal(g.drop_qos1, 2); // a and c to on_puback, b to on_pubcomp; oldest first.
  for (int i = 0; i < 3; i++)
  {
    assert_int_equal(g.drop_ids[i], ids[i]);
    assert_int_equal(g.drop_status[i], AZ_MQTT_ERROR_SESSION_NOT_RESUMED);
  }
  char log[256];
  _take_log(&f, 0, log, (int)sizeof(log));
  assert_string_equal(log, "");
  AZ_MQTT_T(publish_options) const qos1 = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  for (int i = 0; i < 3; i++) // Every entry freed.
  {
    assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, NULL), AZ_OK);
  }
  _teardown(&f);
}

static void a_session_ended_during_not_resumed_reports_stops_them(void** state)
{
  (void)state;
  fixture f;
  uint16_t ids[3];
  _setup_resume(&f, ids, 3);
  test_server_set_session_present(f.server, false);
  g.disconnect_on_drop = true;
  assert_int_not_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(g.drops, 1); // The rest belong to the next CONNACK.
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(g.drops, 3);
  assert_int_equal(g.drop_ids[1], ids[1]);
  assert_int_equal(g.drop_ids[2], ids[2]);
  _teardown(&f);
}

static void publishing_from_a_drop_report_is_safe(void** state)
{
  (void)state;
  fixture f;
  uint16_t ids[3];
  _setup_resume(&f, ids, 6);
  test_server_set_session_present(f.server, false);
  g.publish_on_drop = true;
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  g.publish_on_drop = false;
  assert_int_equal(g.drops, 3); // The old ones only: none published meanwhile.
  char expected[64] = "";
  for (int i = 0; i < 3; i++)
  {
    assert_int_equal(g.drop_ids[i], ids[i]);
    assert_int_equal(g.drop_publish_rc[i], AZ_OK);
    size_t const used = strlen(expected);
    (void)snprintf(expected + used, sizeof(expected) - used, "P1:%u ", g.drop_publish_ids[i]);
  }
  char log[256];
  _take_log(&f, 3, log, (int)sizeof(log)); // Each sent, and kept: resent on a resume.
  assert_string_equal(log, expected);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  test_server_set_session_present(f.server, true);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  _take_log(&f, 3, log, (int)sizeof(log));
  (void)snprintf(
      expected,
      sizeof(expected),
      "P1:%ud P1:%ud P1:%ud ",
      g.drop_publish_ids[0],
      g.drop_publish_ids[1],
      g.drop_publish_ids[2]);
  assert_string_equal(log, expected);
  _teardown(&f);
}

static void no_publish_overtakes_a_pending_resend(void** state)
{
  (void)state;
  test_server_options so = _plain();
  s_persistent = true;
  fixture f;
  _setup(&f, &so, 30); // Never acknowledges.
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  p.message_expiry_interval = 1;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_OK);
  p.message_expiry_interval = 0;
  uint16_t kept;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, &kept), AZ_OK);
  PUMP_UNTIL(&f, test_server_publishes(f.server) == 2);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  char log[256];
  test_server_take_log(f.server, log, (int)sizeof(log));
  _sleep_ms(1100);
  test_server_set_session_present(f.server, true);
  g.publish_on_drop = true; // At the expired one's drop, the kept one still awaits its resend.
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  g.publish_on_drop = false;
  assert_int_equal(g.drops, 1);
  assert_int_equal(g.drop_publish_rc[0], AZ_MQTT_ERROR_FLOW_CONTROL);
  char expected[32];
  (void)snprintf(expected, sizeof(expected), "P1:%ud ", kept);
  _take_log(&f, 1, log, (int)sizeof(log));
  assert_string_equal(log, expected);
  _teardown(&f);
}

static void a_clean_session_needs_no_message_storage(void** state)
{
  (void)state;
  test_server_options so = _plain();
  s_message_storage = 0;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) const qos1 = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  uint16_t id;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, &id), AZ_OK);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(g.drops, 1); // Not resumed: reported.
  assert_int_equal(g.drop_ids[0], id);
  assert_int_equal(g.drop_status[0], AZ_MQTT_ERROR_SESSION_NOT_RESUMED);
  _teardown(&f);
}

static void a_kept_session_needs_message_storage_for_qos_1_and_2(void** state)
{
  (void)state;
  test_server_options so = _plain();
  s_message_storage = 0;
  s_persistent = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_MQTT_ERROR_INVALID_CONFIG);
  p = _publish_options(AZ_MQTT_QOS_EXACTLY_ONCE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_MQTT_ERROR_INVALID_CONFIG);
  p = _publish_options(AZ_MQTT_QOS_AT_MOST_ONCE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_OK);
  _teardown(&f);
}

static void message_storage_below_its_minimum_is_refused(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  s_message_storage = 1024 + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD - 1; // send_buf is 1024 bytes.
  s_init_may_fail = true;
  _setup(&f, &so, 30);
  assert_int_equal(s_init_rc, AZ_MQTT_ERROR_INVALID_CONFIG);
  test_server_stop(f.server);
  free(f.transport);
  s_message_storage = 1024 + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD;
  _setup(&f, &so, 30);
  _teardown(&f);
}

static void full_message_storage_refuses_until_room_is_freed(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.pubrec_only = true;
  s_message_storage = 1024 + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD; // The minimum.
  s_persistent = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  static uint8_t payload[600];
  AZ_MQTT_T(publish_options) big = _publish_options(AZ_MQTT_QOS_EXACTLY_ONCE);
  big.payload = AZ_SPAN_FROM_BUFFER(payload);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &big, NULL), AZ_OK);
  big.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &big, NULL), AZ_MQTT_ERROR_OUT_OF_STORAGE);
  PUMP_UNTIL(&f, test_server_pubrels(f.server) == 1); // PUBREC frees the first copy.
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &big, NULL), AZ_OK);
  _teardown(&f);
}

static void a_publish_over_the_send_buffer_fails_as_before(void** state)
{
  (void)state;
  test_server_options so = _plain();
  s_persistent = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  static uint8_t payload[2000]; // Fits the empty storage, not the 1024-byte send buffer.
  AZ_MQTT_T(publish_options) big = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  big.payload = AZ_SPAN_FROM_BUFFER(payload);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &big, NULL), AZ_ERROR_NOT_ENOUGH_SPACE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &big, NULL), AZ_ERROR_NOT_ENOUGH_SPACE);
  _teardown(&f);
}

static void expired_messages_are_dropped_not_resent(void** state)
{
  (void)state;
  test_server_options so = _plain();
  s_persistent = true;
  fixture f;
  _setup(&f, &so, 30); // Never acknowledges.
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  p.message_expiry_interval = 1;
  uint16_t expiring;
  uint16_t kept;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, &expiring), AZ_OK);
  p.message_expiry_interval = 0;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, &kept), AZ_OK);
  PUMP_UNTIL(&f, test_server_publishes(f.server) == 2);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  char log[256];
  test_server_take_log(f.server, log, (int)sizeof(log));
  _sleep_ms(1100);
  test_server_set_session_present(f.server, true);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(g.drops, 1);
  assert_int_equal(g.drop_ids[0], expiring);
  assert_int_equal(g.drop_status[0], AZ_MQTT_ERROR_MESSAGE_EXPIRED);
  char expected[32];
  (void)snprintf(expected, sizeof(expected), "P1:%ud ", kept);
  _take_log(&f, 1, log, (int)sizeof(log));
  assert_string_equal(log, expected);
  _teardown(&f);
}

static void a_session_ended_during_drops_keeps_the_rest(void** state)
{
  (void)state;
  test_server_options so = _plain();
  s_persistent = true;
  fixture f;
  _setup(&f, &so, 30); // Never acknowledges.
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  p.message_expiry_interval = 1;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_OK);
  p.message_expiry_interval = 0;
  uint16_t kept;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, &kept), AZ_OK);
  PUMP_UNTIL(&f, test_server_publishes(f.server) == 2);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  char log[256];
  test_server_take_log(f.server, log, (int)sizeof(log));
  _sleep_ms(1100);
  test_server_set_session_present(f.server, true);
  g.disconnect_on_drop = true;
  assert_int_not_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(g.drops, 1);
  assert_int_equal(g.connacks, 1); // Only the first connect's: none for the ended one.
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_DISCONNECTED);

  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  char expected[32];
  (void)snprintf(expected, sizeof(expected), "P1:%ud ", kept);
  _take_log(&f, 1, log, (int)sizeof(log));
  assert_string_equal(log, expected);
  _teardown(&f);
}

static void an_inbound_qos2_exchange_resumes(void** state)
{
  (void)state;
  for (int present = 1; present >= 0; present--)
  {
    test_server_options so = _plain();
    so.resume_inbound_qos2 = true;
    s_persistent = true;
    fixture f;
    _setup(&f, &so, 30);
    assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
    PUMP_UNTIL(&f, g.publishes == 1); // PUBLISH 7 received, PUBREC sent.
    assert_int_equal(g.publishes, 1);
    assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
    test_server_set_session_present(f.server, present != 0);
    assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
    PUMP_UNTIL(&f, test_server_pubcomps(f.server) == 1); // DUP resend, then PUBREL 7.
    assert_int_equal(test_server_pubcomps(f.server), 1);
    // Resumed: the resend is a duplicate. Not resumed: a new message, delivered.
    assert_int_equal(g.publishes, present != 0 ? 1 : 2);
    assert_int_equal(g.pubcomps, 1);
    _teardown(&f);
  }
}

/** @brief Wait, without running the client, until the server has read @p count PUBLISH. */
static void _wait_server_publishes(fixture* f, int count)
{
  for (int i = 0; i < 150 && test_server_publishes(f->server) < count; i++)
  {
    _sleep_ms(20);
  }
  assert_int_equal(test_server_publishes(f->server), count);
}

static void resends_keep_their_order_across_identifier_wraps(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.ack_publishes = true;
  so.hold_first_publish = true;
  s_inflight_slots = 3;
  s_persistent = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) const qos1 = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  uint16_t oldest;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, &oldest), AZ_OK); // Held.
  int acked = 0;
  uint16_t id = oldest;
  while (id != 65533)
  {
    assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, &id), AZ_OK);
    acked++;
    PUMP_UNTIL(&f, g.pubacks == acked);
    assert_int_equal(g.pubacks, acked);
  }
  test_server_set_ack_publishes(f.server, false);
  int const seen = test_server_publishes(f.server);
  uint16_t newer;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, &newer), AZ_OK);
  assert_int_equal(newer, 65534);
  PUMP_UNTIL(&f, test_server_publishes(f.server) == seen + 1); // Read while acks are off.
  test_server_set_ack_publishes(f.server, true);
  for (int i = 0; i < 2; i++) // 65535, then 2 (1 is held): newest ids now below both.
  {
    assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, &id), AZ_OK);
    acked++;
    PUMP_UNTIL(&f, g.pubacks == acked);
    assert_int_equal(g.pubacks, acked);
  }
  assert_int_equal(id, 2);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  char log[256];
  test_server_take_log(f.server, log, (int)sizeof(log));

  test_server_set_session_present(f.server, true);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  char expected[64];
  (void)snprintf(expected, sizeof(expected), "P1:%ud P1:%ud ", oldest, newer);
  _take_log(&f, 2, log, (int)sizeof(log));
  assert_string_equal(log, expected);
  _teardown(&f);
}

static void resends_keep_their_order_when_older_entries_were_freed(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.ack_publishes = true;
  s_inflight_slots = 3;
  s_persistent = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) const qos1 = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  uint16_t ids[3];
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, &ids[0]), AZ_OK); // Acknowledged,
  _wait_server_publishes(&f, 1);
  test_server_set_ack_publishes(f.server, false);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, &ids[1]), AZ_OK); // not this one:
  _wait_server_publishes(&f, 2);
  PUMP_UNTIL(&f, g.pubacks == 1); // the oldest entry is freed while a newer one is held,
  assert_int_equal(g.pubacks, 1);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, &ids[2]), AZ_OK); // then this.
  _wait_server_publishes(&f, 3);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  char log[256];
  test_server_take_log(f.server, log, (int)sizeof(log));

  test_server_set_session_present(f.server, true);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  char expected[64];
  (void)snprintf(expected, sizeof(expected), "P1:%ud P1:%ud ", ids[1], ids[2]);
  _take_log(&f, 2, log, (int)sizeof(log));
  assert_string_equal(log, expected);
  _teardown(&f);
}

#if AZ_MQTT_TEST_VERSION == 5
static void resends_wait_for_room_under_the_new_receive_maximum(void** state)
{
  (void)state;
  fixture f;
  uint16_t ids[3];
  _setup_resume(&f, ids, 4);
  test_server_set_receive_maximum(f.server, 2); // The PUBREL and one PUBLISH.
  test_server_set_ack_publishes(f.server, true);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) const qos1 = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  // Not before the resend still waiting.
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, NULL), AZ_MQTT_ERROR_FLOW_CONTROL);
  char expected[64];
  (void)snprintf(expected, sizeof(expected), "R:%u P1:%ud P1:%ud ", ids[1], ids[0], ids[2]);
  char log[256];
  _take_log(&f, 3, log, (int)sizeof(log)); // The last once an acknowledgement makes room.
  assert_string_equal(log, expected);
  _teardown(&f);
}

static void a_resend_carries_the_expiry_interval_left(void** state)
{
  (void)state;
  test_server_options so = _plain();
  s_persistent = true;
  fixture f;
  _setup(&f, &so, 30); // Never acknowledges.
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  p.message_expiry_interval = 10;
  p.payload_format_indicator = 1; // Written before the expiry interval.
  uint16_t id;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, &id), AZ_OK);
  char expected[32];
  (void)snprintf(expected, sizeof(expected), "P1:%ux10 ", id);
  char log[256];
  _take_log(&f, 1, log, (int)sizeof(log));
  assert_string_equal(log, expected);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  _sleep_ms(1100);
  test_server_set_session_present(f.server, true);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  (void)snprintf(expected, sizeof(expected), "P1:%udx9 ", id);
  _take_log(&f, 1, log, (int)sizeof(log));
  assert_string_equal(log, expected);
  _teardown(&f);
}

static void a_resend_over_the_new_maximum_packet_size_is_dropped(void** state)
{
  (void)state;
  test_server_options so = _plain();
  s_persistent = true;
  fixture f;
  _setup(&f, &so, 30); // Never acknowledges.
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  static uint8_t payload[200];
  AZ_MQTT_T(publish_options) p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  p.payload = AZ_SPAN_FROM_BUFFER(payload);
  uint16_t id;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, &id), AZ_OK);
  PUMP_UNTIL(&f, test_server_publishes(f.server) == 1);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  char log[256];
  test_server_take_log(f.server, log, (int)sizeof(log));
  test_server_set_session_present(f.server, true);
  test_server_set_maximum_packet_size(f.server, 100);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(g.drops, 1);
  assert_int_equal(g.drop_ids[0], id);
  assert_int_equal(g.drop_status[0], AZ_MQTT_ERROR_PACKET_TOO_LARGE);
  assert_int_equal(g.last_drop_reason, 0x95);
  _take_log(&f, 0, log, (int)sizeof(log));
  assert_string_equal(log, "");
  _teardown(&f);
}

static void a_resent_pubrel_over_the_new_maximum_packet_size_is_dropped(void** state)
{
  (void)state;
  fixture f;
  uint16_t ids[3];
  _setup_resume(&f, ids, 4);
  test_server_set_maximum_packet_size(f.server, 3); // A PUBREL is 4 bytes.
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  assert_int_equal(g.drops, 3); // The PUBREL first, then both PUBLISH: none fits.
  uint16_t const expected[3] = { ids[1], ids[0], ids[2] };
  for (int i = 0; i < 3; i++)
  {
    assert_int_equal(g.drop_ids[i], expected[i]);
    assert_int_equal(g.drop_status[i], AZ_MQTT_ERROR_PACKET_TOO_LARGE);
  }
  assert_int_equal(g.drop_qos1, 2);
  char log[256];
  _take_log(&f, 0, log, (int)sizeof(log));
  assert_string_equal(log, "");
  // Nothing left to fail the next resume.
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(g.drops, 3);
  _teardown(&f);
}

static void an_acknowledgement_is_reported_before_a_resend_it_makes_room_for(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.pubrec_only = true;
  so.pubcomp_delay_ms = 1500; // The PUBLISH below expires meanwhile.
  s_persistent = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  p.message_expiry_interval = 1;
  uint16_t expiring;
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, &expiring), AZ_OK); // Not acknowledged.
  p = _publish_options(AZ_MQTT_QOS_EXACTLY_ONCE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_OK);
  PUMP_UNTIL(&f, test_server_pubrels(f.server) == 1); // At PUBREL.
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  test_server_set_session_present(f.server, true);
  test_server_set_receive_maximum(f.server, 1); // The PUBREL takes it: the PUBLISH waits.
  test_server_set_ack_publishes(f.server, true);
  g.disconnect_on_drop = true;
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(g.drops, 0);
  // PUBCOMP (late): on_pubcomp, then the now expired PUBLISH is dropped; that report disconnects.
  int64_t const end = _now_ms() + 3000;
  while (AZ_MQTT_T(client_get_state)(&f.client) == AZ_MQTT_CLIENT_STATE_CONNECTED && _now_ms() < end)
  {
    _ignore(AZ_MQTT_T(client_process_loop)(&f.client, 20));
  }
  assert_int_equal(g.pubcomps, 1);
  assert_int_equal(g.drops, 1);
  assert_int_equal(g.drop_ids[0], expiring);
  assert_int_equal(g.drop_status[0], AZ_MQTT_ERROR_MESSAGE_EXPIRED);
  _teardown(&f);
}

static void a_topic_alias_is_refused_on_a_kept_session(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.topic_alias_maximum = 5;
  s_persistent = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  p.topic_alias = 1; // A stored copy must not depend on this connection.
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_MQTT_ERROR_NOT_SUPPORTED);
  p.qos = AZ_MQTT_QOS_AT_MOST_ONCE; // Not stored: allowed.
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_OK);
  _teardown(&f);
}
#endif

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
  assert_int_equal(g.incoming_pubcomps, 2);
  assert_int_equal(test_server_last_pubcomp_reason(f.server), 0);
  _teardown(&f);
}

#if AZ_MQTT_TEST_VERSION == 5
static void requests_leave_the_receive_maximum_to_inbound_qos2(void** state)
{
  (void)state;
  test_server_options so = _plain(); // Never acknowledges a PUBLISH.
  so.send_qos2_sequence = true;      // Queued right after CONNACK; read by the pumps below.
  s_inflight_slots = 4;
  s_receive_maximum = 2;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(publish_options) const qos1 = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, NULL), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, NULL), AZ_OK);
  // 2 of 4 entries left for the 2 inbound QoS 2 the server may send.
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &qos1, NULL), AZ_MQTT_ERROR_FLOW_CONTROL);
  PUMP_UNTIL(&f, test_server_pubcomps(f.server) == 2);
  assert_int_equal(test_server_pubcomps(f.server), 2);
  assert_int_equal(g.publishes, 2);
  assert_int_equal(g.closed, 0);
  _teardown(&f);
}
#endif

static void inbound_qos2_without_a_free_slot_ends_the_session(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.send_qos2_sequence = true;
  s_inflight_slots = 0;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  az_result rc = AZ_OK;
  for (int i = 0; i < 150 && az_result_succeeded(rc); i++)
  {
    rc = AZ_MQTT_T(client_process_loop)(&f.client, 20);
  }
  assert_int_equal(rc, AZ_MQTT_ERROR_FLOW_CONTROL);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_FLOW_CONTROL);
  assert_int_equal(g.publishes, 0); // Not delivered, so a resend cannot be delivered twice.
  assert_int_equal(test_server_pubcomps(f.server), 0);
#if AZ_MQTT_TEST_VERSION == 5
  int64_t const end = _now_ms() + 2000; // The server thread records the DISCONNECT.
  while (test_server_client_disconnect_reason(f.server) < 0 && _now_ms() < end)
  {
    _sleep_ms(10);
  }
  assert_int_equal(test_server_client_disconnect_reason(f.server), 0x97); // Quota exceeded
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

/** @brief The server sends @p bytes after CONNACK; the session must close as malformed. */
static void _expect_malformed(uint8_t const* bytes, int size)
{
  test_server_options so = _plain();
  so.raw_after_connack = bytes;
  so.raw_after_connack_size = size;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  az_result rc = AZ_OK;
  for (int i = 0; i < 150 && az_result_succeeded(rc); i++)
  {
    rc = AZ_MQTT_T(client_process_loop)(&f.client, 20);
  }
  assert_int_equal(rc, AZ_MQTT_ERROR_MALFORMED_PACKET);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_MALFORMED_PACKET);
  assert_int_equal(test_server_pubcomps(f.server), 0); // Not answered.
  _teardown(&f);
}

static void a_pubrel_without_its_required_flags_ends_the_session(void** state)
{
  (void)state;
  static const uint8_t pubrel_flags_0[] = { 0x60, 0x02, 0x00, 0x07 };
  _expect_malformed(pubrel_flags_0, (int)sizeof(pubrel_flags_0));
}

static void a_puback_with_flags_set_ends_the_session(void** state)
{
  (void)state;
  static const uint8_t puback_flags_2[] = { 0x42, 0x02, 0x00, 0x07 };
  _expect_malformed(puback_flags_2, (int)sizeof(puback_flags_2));
}

static void wrong_flags_are_rejected_before_the_remaining_length_is_read(void** state)
{
  (void)state;
  static const uint8_t puback_flags_1_alone[] = { 0x41 }; // Nothing follows.
  _expect_malformed(puback_flags_1_alone, (int)sizeof(puback_flags_1_alone));
}

#if AZ_MQTT_TEST_VERSION == 5
static void an_auth_with_a_malformed_string_ends_the_session(void** state)
{
  (void)state;
  // Continue authentication; Reason String "a\0b".
  static const uint8_t auth[] = { 0xF0, 0x08, 0x18, 0x06, 0x1F, 0x00, 0x03, 'a', 0x00, 'b' };
  _expect_malformed(auth, (int)sizeof(auth));
}

#endif

#if AZ_MQTT_TEST_VERSION == 5
static void a_topic_alias_above_the_advertised_maximum_ends_the_session(void** state)
{
  (void)state;
  // QoS 0 PUBLISH "t" with Topic Alias 1; the client advertised Topic Alias Maximum 0.
  static const uint8_t publish[] = { 0x30, 0x07, 0x00, 0x01, 't', 0x03, 0x23, 0x00, 0x01 };
  test_server_options so = _plain();
  so.raw_after_connack = publish;
  so.raw_after_connack_size = (int)sizeof(publish);
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  az_result rc = AZ_OK;
  for (int i = 0; i < 150 && az_result_succeeded(rc); i++)
  {
    rc = AZ_MQTT_T(client_process_loop)(&f.client, 20);
  }
  assert_int_equal(rc, AZ_MQTT_ERROR_PROTOCOL);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_PROTOCOL);
  assert_int_equal(g.publishes, 0);
  int64_t const end = _now_ms() + 2000; // The server thread records the DISCONNECT.
  while (test_server_client_disconnect_reason(f.server) < 0 && _now_ms() < end)
  {
    _sleep_ms(10);
  }
  assert_int_equal(test_server_client_disconnect_reason(f.server), 0x94); // Topic Alias invalid
  _teardown(&f);
}
#endif

static void a_pingresp_with_a_body_ends_the_session(void** state)
{
  (void)state;
  static const uint8_t pingresp[] = { 0xD0, 0x01, 0x00 };
  _expect_malformed(pingresp, (int)sizeof(pingresp));
}

#if AZ_MQTT_TEST_VERSION == 5
static void an_acknowledgement_with_a_disallowed_reason_ends_the_session(void** state)
{
  (void)state;
  // PUBACK, PUBREC, PUBREL, PUBCOMP with reason 0x01 (allowed in SUBACK only).
  static const uint8_t acks[][5] = { { 0x40, 0x03, 0x00, 0x07, 0x01 },
                                     { 0x50, 0x03, 0x00, 0x07, 0x01 },
                                     { 0x62, 0x03, 0x00, 0x07, 0x01 },
                                     { 0x70, 0x03, 0x00, 0x07, 0x01 } };
  for (size_t k = 0; k < sizeof(acks) / sizeof(acks[0]); k++)
  {
    test_server_options so = _plain();
    so.raw_after_connack = acks[k];
    so.raw_after_connack_size = (int)sizeof(acks[k]);
    fixture f;
    _setup(&f, &so, 30);
    assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
    az_result rc = AZ_OK;
    for (int i = 0; i < 150 && az_result_succeeded(rc); i++)
    {
      rc = AZ_MQTT_T(client_process_loop)(&f.client, 20);
    }
    assert_int_equal(rc, AZ_MQTT_ERROR_PROTOCOL);
    assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_PROTOCOL);
    _teardown(&f);
  }
}
#endif

static void a_publish_to_an_empty_or_wildcard_topic_is_refused(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  char const* const topics[] = { "", "a/+", "a/#" };
  for (size_t i = 0; i < sizeof(topics) / sizeof(topics[0]); i++)
  {
    AZ_MQTT_T(publish_options) p = _publish_options(AZ_MQTT_QOS_AT_MOST_ONCE);
    p.topic = az_span_create_from_str((char*)(uintptr_t)topics[i]);
    assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_ERROR_ARG);
  }
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  assert_int_equal(test_server_publishes(f.server), 0);
  _teardown(&f);
}

static void a_second_connack_ends_the_session(void** state)
{
  (void)state;
#if AZ_MQTT_TEST_VERSION == 5
  static const uint8_t connack[] = { 0x20, 0x03, 0x00, 0x00, 0x00 };
#else
  static const uint8_t connack[] = { 0x20, 0x02, 0x00, 0x00 };
#endif
  test_server_options so = _plain();
  so.raw_after_connack = connack;
  so.raw_after_connack_size = (int)sizeof(connack);
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  az_result rc = AZ_OK;
  for (int i = 0; i < 150 && az_result_succeeded(rc); i++)
  {
    rc = AZ_MQTT_T(client_process_loop)(&f.client, 20);
  }
  assert_int_equal(rc, AZ_MQTT_ERROR_PROTOCOL);
  assert_int_equal(g.connacks, 1);
  _teardown(&f);
}

static void a_subscription_to_an_empty_filter_is_refused(void** state)
{
  (void)state;
  test_server_options so = _plain();
  s_inflight_slots = 1; // A refused request that kept its entry would block the next.
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(subscription) sub;
  memset(&sub, 0, sizeof(sub));
  assert_int_equal(AZ_MQTT_T(client_subscribe)(&f.client, &sub, 1, NULL), AZ_ERROR_ARG);
  az_span const empty = AZ_SPAN_EMPTY;
  assert_int_equal(AZ_MQTT_T(client_unsubscribe)(&f.client, &empty, 1, NULL), AZ_ERROR_ARG);
  assert_int_equal(_subscribe(&f), AZ_OK);
  _teardown(&f);
}

static void a_publish_with_a_malformed_topic_is_refused(void** state)
{
  (void)state;
  test_server_options so = _plain();
  s_inflight_slots = 1; // A refused QoS 1 PUBLISH that kept its entry would block the next.
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  for (int qos = 0; qos <= 1; qos++)
  {
    AZ_MQTT_T(publish_options) p = _publish_options((az_mqtt_qos)qos);
    p.topic = az_span_create((uint8_t*)(uintptr_t) "\xC0\x80", 2); // Overlong U+0000.
    assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_ERROR_ARG);
  }
  // Nothing sent, no in-flight entry kept, and the session is up.
  AZ_MQTT_T(publish_options) const p = _publish_options(AZ_MQTT_QOS_AT_LEAST_ONCE);
  assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_OK);
  PUMP_UNTIL(&f, test_server_publishes(f.server) == 1);
  assert_int_equal(test_server_publishes(f.server), 1);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  _teardown(&f);
}

static void a_topic_with_u0000_ends_the_session(void** state)
{
  (void)state;
#if AZ_MQTT_TEST_VERSION == 5
  static const uint8_t publish[] = { 0x30, 0x06, 0x00, 0x03, 'a', 0x00, 'b', 0x00 };
#else
  static const uint8_t publish[] = { 0x30, 0x05, 0x00, 0x03, 'a', 0x00, 'b' };
#endif
  _expect_malformed(publish, (int)sizeof(publish));
  assert_int_equal(g.publishes, 0);
}

static void wrong_flags_are_rejected_before_the_body_is_read(void** state)
{
  (void)state;
  // Remaining Length 268,435,455 (more than the receive buffer); no body follows.
  static const uint8_t puback_flags_1_huge[] = { 0x41, 0xFF, 0xFF, 0xFF, 0x7F };
  _expect_malformed(puback_flags_1_huge, (int)sizeof(puback_flags_1_huge));
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

static void native_errors_reach_the_client_before_the_session_closes(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 30);
  test_server_stop(f.server); // Its port now refuses connections.
  f.server = NULL;
  for (uint32_t attempt = 1; attempt <= 2; attempt++)
  {
    test_native_errors_clear(&g.native);
    assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_MQTT_ERROR_CONNECTION_REFUSED);
    assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_CONNECTION_REFUSED);
    assert_int_equal(g.native.count, 1);
    assert_int_equal(g.native_at_close, 1); // Reported before on_connection_closed.
    assert_ptr_equal(g.native_client, &f.client);
    assert_int_equal(g.native.errors[0].source, AZ_MQTT_NATIVE_ERROR_SOCKET);
    assert_int_equal(g.native.errors[0].code, ECONNREFUSED);
    assert_true(
        test_native_errors_all_belong_to(&g.native, AZ_MQTT_ERROR_CONNECTION_REFUSED, attempt));
  }
  _teardown(&f);
}

static void invalid_proxy_options_fail_initialization(void** state)
{
  (void)state;
  az_mqtt_proxy_options bad;
  memset(&bad, 0, sizeof(bad));
  bad.host = AZ_SPAN_FROM_STR("127.0.0.1");
  bad.port = 0;
  fixture f;
  memset(&f, 0, sizeof(f));
  f.transport = (az_mqtt_transport*)calloc(1, (size_t)az_mqtt_transport_sizeof());
  assert_non_null(f.transport);
  assert_int_equal(az_mqtt_transport_init(f.transport), AZ_OK);
  AZ_MQTT_T(client_options) o;
  memset(&o, 0, sizeof(o));
  o.transport = f.transport;
  o.proxy_options = &bad;
#ifndef AZ_MQTT_NO_PROXY
  assert_int_equal(AZ_MQTT_T(client_init)(&f.client, &o), AZ_MQTT_ERROR_INVALID_CONFIG);
#else
  assert_int_equal(AZ_MQTT_T(client_init)(&f.client, &o), AZ_MQTT_ERROR_NOT_SUPPORTED);
#endif
  free(f.transport);
}

#ifndef AZ_MQTT_NO_WEBSOCKETS
static test_server_options _ws(void)
{
  test_server_options o = _plain();
  o.websocket = true;
  return o;
}

/** @brief Subscribe and wait for the SUBACK. */
static void _subscribe_and_wait(fixture* f)
{
  AZ_MQTT_T(subscription) sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR("t");
  assert_int_equal(AZ_MQTT_T(client_subscribe)(&f->client, &sub, 1, NULL), AZ_OK);
  int64_t const end = _now_ms() + 3000;
  while (g.subacks == 0 && _now_ms() < end)
  {
    assert_int_equal(AZ_MQTT_T(client_process_loop)(&f->client, 20), AZ_OK);
  }
  assert_int_equal(g.subacks, 1);
}

static void a_websocket_session_upgrades_masks_and_closes_cleanly(void** state)
{
  (void)state;
#if defined(AZ_MQTT_TEST_BACKEND_NONE)
  int const tls_cases = 1;
#else
  int const tls_cases = 2;
#endif
  for (int tls = 0; tls < tls_cases; tls++)
  {
    test_server_options so = _ws();
    so.tls = tls == 1;
    so.ack_publishes = true;
    fixture f;
    _setup(&f, &so, 30);
    assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
    char host[48];
    snprintf(host, sizeof(host), "\r\nHost: 127.0.0.1:%u\r\n", test_server_port(f.server));
    assert_true(test_server_ws_request_has(f.server, "GET /mqtt HTTP/1.1\r\n"));
    assert_true(test_server_ws_request_has(f.server, host));
    assert_true(test_server_ws_request_has(f.server, "\r\nSec-WebSocket-Protocol: mqtt\r\n"));
    _subscribe_and_wait(&f);
    AZ_MQTT_T(publish_options) p = AZ_MQTT_T(publish_options_default)();
    p.topic = AZ_SPAN_FROM_STR("t");
    static uint8_t payload[900]; // Over AZ_MQTT_WEBSOCKET_SEND_CHUNK.
    p.payload = AZ_SPAN_FROM_BUFFER(payload);
    p.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
    assert_int_equal(AZ_MQTT_T(client_publish)(&f.client, &p, NULL), AZ_OK);
    int64_t const end = _now_ms() + 3000;
    while (g.pubacks == 0 && _now_ms() < end)
    {
      assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 20), AZ_OK);
    }
    assert_int_equal(g.pubacks, 1);
    assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
    for (int i = 0; i < 100 && test_server_ws_client_close_code(f.server) < 0; i++)
    {
      _sleep_ms(10);
    }
    assert_int_equal(test_server_ws_client_close_code(f.server), 1000);
    assert_int_equal(test_server_ws_unmasked(f.server), 0);
    assert_int_equal(g.native.count, 0);
    _teardown(&f);
  }
}

static void fragmented_frames_and_pings_are_handled(void** state)
{
  (void)state;
  test_server_options so = _ws();
  so.ws_fragment = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  _subscribe_and_wait(&f);
  assert_true(test_server_ws_pongs(f.server) >= 4); // CONNACK's 4 bytes at least.
  assert_int_equal(test_server_ws_unmasked(f.server), 0);
  _teardown(&f);
}

static void a_slow_long_upgrade_reply_completes_across_calls(void** state)
{
  (void)state;
  test_server_options so = _ws();
  so.ws_reply = TEST_SERVER_WS_SLOW_LONG_REPLY; // Then a ping in the same stream.
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect_start)(&f.client, 3000), AZ_OK);
  assert_int_equal(_pump_connect(&f, 3000), AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  _subscribe_and_wait(&f);
  assert_int_equal(test_server_ws_pongs(f.server), 1);
  _teardown(&f);
}

static void a_refused_or_invalid_upgrade_fails_the_connect(void** state)
{
  (void)state;
  test_server_ws_reply const replies[] = { TEST_SERVER_WS_REFUSE, TEST_SERVER_WS_BAD_ACCEPT };
  int const statuses[] = { 403, 101 };
  for (int i = 0; i < 2; i++)
  {
    test_server_options so = _ws();
    so.ws_reply = replies[i];
    fixture f;
    _setup(&f, &so, 30);
    for (uint32_t attempt = 1; attempt <= 2; attempt++)
    {
      test_native_errors_clear(&g.native);
      assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_MQTT_ERROR_WEBSOCKET);
      assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_WEBSOCKET);
      assert_int_equal(g.native.count, 1);
      assert_int_equal(g.native_at_close, 1);
      assert_int_equal(g.native.errors[0].source, AZ_MQTT_NATIVE_ERROR_WEBSOCKET);
      assert_int_equal(g.native.errors[0].code, statuses[i]);
      assert_int_equal(g.native.errors[0].result, AZ_MQTT_ERROR_WEBSOCKET);
      assert_int_equal(g.native.errors[0].connect_attempt, attempt);
    }
    assert_int_equal(g.connacks, 0);
    _teardown(&f);
  }
}

static void a_server_close_frame_ends_the_session(void** state)
{
  (void)state;
  test_server_options so = _ws();
  so.ws_close_code = 1001;
  fixture f;
  _setup(&f, &so, 30);
  az_result const rc = AZ_MQTT_T(client_connect)(&f.client, 3000);
  assert_true(rc == AZ_OK || rc == AZ_MQTT_ERROR_CONNECTION_CLOSED);
  _ignore(_pump_until_closed(&f, 3000));
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_CONNECTION_CLOSED);
  assert_int_equal(g.native.count, 1);
  assert_int_equal(g.native.errors[0].source, AZ_MQTT_NATIVE_ERROR_WEBSOCKET);
  assert_int_equal(g.native.errors[0].code, 1001);
  assert_int_equal(g.native.errors[0].result, AZ_MQTT_ERROR_CONNECTION_CLOSED);
  for (int i = 0; i < 100 && test_server_ws_client_close_code(f.server) < 0; i++)
  {
    _sleep_ms(10);
  }
  assert_int_equal(test_server_ws_client_close_code(f.server), 1001); // Echoed.
  _teardown(&f);
}

static void a_masked_server_frame_is_refused(void** state)
{
  (void)state;
  test_server_options so = _ws();
  so.ws_masked_frame = true;
  fixture f;
  _setup(&f, &so, 30);
  az_result const rc = AZ_MQTT_T(client_connect)(&f.client, 3000);
  assert_true(rc == AZ_OK || rc == AZ_MQTT_ERROR_WEBSOCKET);
  _ignore(_pump_until_closed(&f, 3000));
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_WEBSOCKET);
  assert_int_equal(g.native.count, 1);
  assert_int_equal(g.native.errors[0].source, AZ_MQTT_NATIVE_ERROR_WEBSOCKET);
  assert_int_equal(g.native.errors[0].code, 1002);
  _teardown(&f);
}

static void invalid_websocket_options_fail_initialization(void** state)
{
  (void)state;
  az_mqtt_transport* const transport
      = (az_mqtt_transport*)calloc(1, (size_t)az_mqtt_transport_sizeof());
  assert_non_null(transport);
  assert_int_equal(az_mqtt_transport_init(transport), AZ_OK);
  az_mqtt_websocket ws;
  az_mqtt_websocket_options options = az_mqtt_websocket_options_default();
  options.path = AZ_SPAN_FROM_STR("mqtt");
  assert_int_equal(az_mqtt_websocket_init(&ws, transport, &options), AZ_MQTT_ERROR_INVALID_CONFIG);
  free(transport);
}
#else
static void websockets_are_refused_without_websocket_support(void** state)
{
  (void)state;
  az_mqtt_transport* const transport
      = (az_mqtt_transport*)calloc(1, (size_t)az_mqtt_transport_sizeof());
  assert_non_null(transport);
  assert_int_equal(az_mqtt_transport_init(transport), AZ_OK);
  az_mqtt_websocket ws;
  assert_int_equal(az_mqtt_websocket_init(&ws, transport, NULL), AZ_MQTT_ERROR_NOT_SUPPORTED);
  free(transport);
}
#endif // AZ_MQTT_NO_WEBSOCKETS

#if !defined(AZ_MQTT_TEST_BACKEND_NONE)
/** @brief Wait up to 2 s for the server to count @p n close_notify. */
static int _close_notifies(fixture* f, int n)
{
  for (int i = 0; i < 200 && test_server_close_notifies(f->server) < n; i++)
  {
    _sleep_ms(10);
  }
  return test_server_close_notifies(f->server);
}

static void tls_sessions_end_with_close_notify_whatever_the_reason(void** state)
{
  (void)state;
  // Orderly disconnect.
  test_server_options so = test_server_options_default();
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  assert_int_equal(_close_notifies(&f, 1), 1);
  _teardown(&f);

  // Keep-alive timeout: the session failed, the connection did not.
  so = test_server_options_default();
  so.no_pingresp = true;
  _setup(&f, &so, 1);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(_pump_until_closed(&f, 5000), AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT);
  assert_int_equal(_close_notifies(&f, 1), 1);
  assert_int_equal(g.native.count, 0); // Sent without error.
  _teardown(&f);

  // Protocol error.
  so = test_server_options_default();
  so.send_auth = true; // Reserved in MQTT 3.1.1; unsolicited in 5.
  _setup(&f, &so, 30);
  _ignore(AZ_MQTT_T(client_connect)(&f.client, 3000));
  _ignore(_pump_until_closed(&f, 1000));
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_PROTOCOL);
  assert_int_equal(_close_notifies(&f, 1), 1);
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
#ifndef AZ_MQTT_NO_WEBSOCKETS
  so.websocket = true;
#endif
  s_with_secrets = true;
#ifndef AZ_MQTT_NO_PROXY
  test_proxy_options po = { 0 };
  po.username = "SECRET-PROXY-USER";
  po.password = "SECRET-PROXY-PASSWORD";
  test_proxy* proxy = test_proxy_start(&po);
  assert_non_null(proxy);
  az_mqtt_proxy_options proxy_options;
  memset(&proxy_options, 0, sizeof(proxy_options));
  proxy_options.host = AZ_SPAN_FROM_STR("127.0.0.1");
  proxy_options.port = test_proxy_port(proxy);
  proxy_options.username = AZ_SPAN_FROM_STR("SECRET-PROXY-USER");
  proxy_options.password = AZ_SPAN_FROM_STR("SECRET-PROXY-PASSWORD");
  s_proxy = &proxy_options;
#endif
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
#ifndef AZ_MQTT_NO_PROXY
  assert_int_equal(test_proxy_tunnels(proxy), 1);
#endif
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
#ifndef AZ_MQTT_NO_PROXY
  char via[48];
  snprintf(via, sizeof(via), " via 127.0.0.1:%u\n", test_proxy_port(proxy));
  test_proxy_stop(proxy);
  assert_non_null(strstr(s_log, via)); // The proxy, never its credentials.
#if !defined(AZ_MQTT_TEST_BACKEND_NONE)
  assert_non_null(strstr(s_log, " tls via "));
#endif
#endif

  assert_non_null(strstr(s_log, "sent CONNECT "));
  assert_non_null(strstr(s_log, "received CONNACK "));
  assert_non_null(strstr(s_log, "sent SUBSCRIBE "));
  assert_non_null(strstr(s_log, "received PUBACK "));
  assert_non_null(strstr(s_log, "closed 0x00010000\n"));
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

static void acknowledgement_codes_must_match_the_filters(void** state)
{
  (void)state;
  AZ_MQTT_T(subscription) subs[2];
  memset(subs, 0, sizeof(subs));
  subs[0].topic_filter = AZ_SPAN_FROM_STR("a");
  subs[1].topic_filter = AZ_SPAN_FROM_STR("b");
  // SUBACK: 2 codes for 1 filter, then 1 code for 2 filters.
  for (int run = 0; run < 2; run++)
  {
    test_server_options so = _plain();
    so.suback_codes = 2 - run;
    fixture f;
    _setup(&f, &so, 30);
    assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
    assert_int_equal(AZ_MQTT_T(client_subscribe)(&f.client, subs, 1 + run, NULL), AZ_OK);
    az_result rc = AZ_OK;
    for (int i = 0; i < 40 && rc == AZ_OK; i++)
    {
      rc = AZ_MQTT_T(client_process_loop)(&f.client, 50);
    }
    assert_int_equal(rc, AZ_MQTT_ERROR_PROTOCOL);
    assert_int_equal(g.subacks, 0);
    _teardown(&f);
  }
#if AZ_MQTT_TEST_VERSION == 5
  // UNSUBACK: 1 code for 2 filters.
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  az_span const filters[] = { AZ_SPAN_FROM_STR("a"), AZ_SPAN_FROM_STR("b") };
  assert_int_equal(AZ_MQTT_T(client_unsubscribe)(&f.client, filters, 2, NULL), AZ_OK);
  az_result rc = AZ_OK;
  for (int i = 0; i < 40 && rc == AZ_OK; i++)
  {
    rc = AZ_MQTT_T(client_process_loop)(&f.client, 50);
  }
  assert_int_equal(rc, AZ_MQTT_ERROR_PROTOCOL);
  _teardown(&f);
#endif
}

static void suback_reason_codes_never_exceed_the_buffer(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.suback_codes = 10; // The fixture holds 4.
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(subscription) subs[10];
  memset(subs, 0, sizeof(subs));
  for (int i = 0; i < 10; i++)
  {
    subs[i].topic_filter = AZ_SPAN_FROM_STR("t");
  }
  assert_int_equal(AZ_MQTT_T(client_subscribe)(&f.client, subs, 10, NULL), AZ_OK);
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

static void an_auth_packet_is_a_protocol_error(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.send_auth = true; // Reserved in MQTT 3.1.1; unsolicited in 5 (no enhanced authentication).
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  az_result rc = AZ_OK;
  for (int i = 0; i < 10 && rc == AZ_OK; i++)
  {
    rc = AZ_MQTT_T(client_process_loop)(&f.client, 50);
  }
  assert_int_equal(rc, AZ_MQTT_ERROR_PROTOCOL);
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
#if !defined(AZ_MQTT_TEST_BACKEND_NONE)
    cmocka_unit_test(tls_sessions_end_with_close_notify_whatever_the_reason),
#endif
#ifndef AZ_MQTT_NO_WEBSOCKETS
    cmocka_unit_test(a_websocket_session_upgrades_masks_and_closes_cleanly),
    cmocka_unit_test(fragmented_frames_and_pings_are_handled),
    cmocka_unit_test(a_slow_long_upgrade_reply_completes_across_calls),
    cmocka_unit_test(a_refused_or_invalid_upgrade_fails_the_connect),
    cmocka_unit_test(a_server_close_frame_ends_the_session),
    cmocka_unit_test(a_masked_server_frame_is_refused),
    cmocka_unit_test(invalid_websocket_options_fail_initialization),
#else
    cmocka_unit_test(websockets_are_refused_without_websocket_support),
#endif
    cmocka_unit_test(an_idle_session_with_answered_pings_stays_up),
    cmocka_unit_test(a_missing_pingresp_ends_the_session),
    cmocka_unit_test(server_keep_alive_overrides_the_clients),
    cmocka_unit_test(a_server_disconnect_closes_the_connection),
    cmocka_unit_test(one_process_loop_drains_a_burst),
#if !defined(AZ_MQTT_TEST_BACKEND_NONE)
    cmocka_unit_test(data_after_a_session_ticket_is_read_without_waiting),
#endif
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
    cmocka_unit_test(a_resumed_session_resends_pubrel_then_publishes_oldest_first),
    cmocka_unit_test(without_session_present_each_exchange_is_reported_dropped),
    cmocka_unit_test(a_session_ended_during_not_resumed_reports_stops_them),
    cmocka_unit_test(publishing_from_a_drop_report_is_safe),
    cmocka_unit_test(no_publish_overtakes_a_pending_resend),
    cmocka_unit_test(a_clean_session_needs_no_message_storage),
    cmocka_unit_test(a_kept_session_needs_message_storage_for_qos_1_and_2),
    cmocka_unit_test(message_storage_below_its_minimum_is_refused),
    cmocka_unit_test(full_message_storage_refuses_until_room_is_freed),
    cmocka_unit_test(a_publish_over_the_send_buffer_fails_as_before),
    cmocka_unit_test(expired_messages_are_dropped_not_resent),
    cmocka_unit_test(a_session_ended_during_drops_keeps_the_rest),
    cmocka_unit_test(an_inbound_qos2_exchange_resumes),
    cmocka_unit_test(resends_keep_their_order_across_identifier_wraps),
    cmocka_unit_test(resends_keep_their_order_when_older_entries_were_freed),
#if AZ_MQTT_TEST_VERSION == 5
    cmocka_unit_test(resends_wait_for_room_under_the_new_receive_maximum),
    cmocka_unit_test(a_resend_carries_the_expiry_interval_left),
    cmocka_unit_test(a_resend_over_the_new_maximum_packet_size_is_dropped),
    cmocka_unit_test(a_resent_pubrel_over_the_new_maximum_packet_size_is_dropped),
    cmocka_unit_test(an_acknowledgement_is_reported_before_a_resend_it_makes_room_for),
    cmocka_unit_test(a_topic_alias_is_refused_on_a_kept_session),
#endif
    cmocka_unit_test(inbound_qos2_duplicates_are_delivered_once),
    cmocka_unit_test(inbound_qos2_without_a_free_slot_ends_the_session),
#if AZ_MQTT_TEST_VERSION == 5
    cmocka_unit_test(requests_leave_the_receive_maximum_to_inbound_qos2),
#endif
    cmocka_unit_test(unknown_acknowledgements_are_ignored),
    cmocka_unit_test(a_pubrel_without_its_required_flags_ends_the_session),
    cmocka_unit_test(a_puback_with_flags_set_ends_the_session),
    cmocka_unit_test(wrong_flags_are_rejected_before_the_body_is_read),
    cmocka_unit_test(a_topic_with_u0000_ends_the_session),
    cmocka_unit_test(a_publish_with_a_malformed_topic_is_refused),
    cmocka_unit_test(a_second_connack_ends_the_session),
    cmocka_unit_test(a_subscription_to_an_empty_filter_is_refused),
    cmocka_unit_test(a_publish_to_an_empty_or_wildcard_topic_is_refused),
    cmocka_unit_test(a_pingresp_with_a_body_ends_the_session),
#if AZ_MQTT_TEST_VERSION == 5
    cmocka_unit_test(an_acknowledgement_with_a_disallowed_reason_ends_the_session),
#endif
#if AZ_MQTT_TEST_VERSION == 5
    cmocka_unit_test(a_topic_alias_above_the_advertised_maximum_ends_the_session),
#endif
#if AZ_MQTT_TEST_VERSION == 5
    cmocka_unit_test(an_auth_with_a_malformed_string_ends_the_session),
#endif
    cmocka_unit_test(wrong_flags_are_rejected_before_the_remaining_length_is_read),
#if AZ_MQTT_TEST_VERSION == 5
    cmocka_unit_test(the_server_receive_maximum_limits_publishes),
    cmocka_unit_test(the_server_limits_are_enforced),
    cmocka_unit_test(an_acknowledgement_over_the_server_maximum_packet_size_closes),
    cmocka_unit_test(a_failed_pubrec_ends_the_exchange),
#endif
    cmocka_unit_test(native_errors_reach_the_client_before_the_session_closes),
    cmocka_unit_test(invalid_proxy_options_fail_initialization),
    cmocka_unit_test(a_peer_close_is_reported_as_connection_closed),
#ifndef AZ_NO_LOGGING
    cmocka_unit_test(logs_never_contain_credentials_topics_or_payloads),
#endif
    cmocka_unit_test(reconnect_after_a_lost_session_works),
    cmocka_unit_test(a_refused_connack_code_is_reported_verbatim),
    cmocka_unit_test(suback_reason_codes_never_exceed_the_buffer),
    cmocka_unit_test(acknowledgement_codes_must_match_the_filters),
#if AZ_MQTT_TEST_VERSION == 5
    cmocka_unit_test(publish_properties_never_exceed_the_buffers),
#endif
    cmocka_unit_test(an_auth_packet_is_a_protocol_error),
    cmocka_unit_test(a_server_disconnect_is_a_protocol_error_only_in_mqttv3),
    cmocka_unit_test(an_explicit_server_keep_alive_of_zero_disables_pings),
    cmocka_unit_test(reconnecting_from_on_connection_closed_is_safe),
  };
  return cmocka_run_group_tests_name("client_session", tests, NULL, NULL);
}
