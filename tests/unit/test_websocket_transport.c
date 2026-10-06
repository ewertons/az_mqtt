// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_websocket_transport.c
 * @brief Unit tests: the WebSocket transport layer over an in-memory transport.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "az_mqtt_websocket_internal.h"
#include "test_fake_transport.h"

#include <az_mqtt/az_mqtt_websocket.h>

#include <azure/core/az_base64.h>

#include <string.h>

typedef struct
{
  test_fake_transport fake;
  az_mqtt_websocket ws;
  az_mqtt_transport* t;
} fixture;

static uint8_t s_input[8192];
static uint8_t s_sent[220000];
static int32_t s_sent_read; ///< Client frames already decoded from s_sent.

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

/** @brief Fixture over a fresh fake; the WebSocket's native errors go to s_native. */
static void _init(fixture* f, az_mqtt_websocket_options const* options)
{
  memset(s_input, 0, sizeof(s_input));
  memset(s_sent, 0, sizeof(s_sent));
  s_sent_read = 0;
  memset(&s_native, 0, sizeof(s_native));
  test_fake_transport_init(
      &f->fake, AZ_SPAN_FROM_BUFFER(s_input), az_span_create(s_sent, (int32_t)sizeof(s_sent) - 1));
  assert_int_equal(az_mqtt_websocket_init(&f->ws, &f->fake.layer.base, options), AZ_OK);
  f->t = az_mqtt_websocket_get_transport(&f->ws);
  az_mqtt_transport_set_error_callback(f->t, _on_native, NULL);
}

/** @brief The Sec-WebSocket-Accept for the key in the request sent. */
static void _accept_for_request(char out[29])
{
  char const* key = strstr((char const*)s_sent, "\r\nSec-WebSocket-Key: ");
  assert_non_null(key);
  key += 21;
  static char const guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t joined[24 + sizeof(guid) - 1];
  memcpy(joined, key, 24);
  memcpy(joined + 24, guid, sizeof(guid) - 1);
  uint8_t digest[20];
  _az_mqtt_sha1(joined, sizeof(joined), digest);
  int32_t written = 0;
  assert_int_equal(
      az_base64_encode(az_span_create((uint8_t*)out, 28), AZ_SPAN_FROM_BUFFER(digest), &written),
      AZ_OK);
  out[28] = '\0';
}

/** @brief Append a server frame (unmasked) to the input. */
static void _feed_frame(fixture* f, uint8_t b0, void const* payload, int32_t size)
{
  uint8_t header[4] = { b0, (uint8_t)size };
  int32_t n = 2;
  if (size >= 126)
  {
    header[1] = 126;
    header[2] = (uint8_t)(size >> 8);
    header[3] = (uint8_t)size;
    n = 4;
  }
  test_fake_transport_feed(&f->fake, header, n);
  test_fake_transport_feed(&f->fake, payload, size);
}

/** @brief Poll the connect with no wait until it is not AZ_MQTT_ERROR_TIMEOUT. */
static az_result _poll(fixture* f)
{
  az_result rc = AZ_MQTT_ERROR_TIMEOUT;
  for (int i = 0; i < 20000 && rc == AZ_MQTT_ERROR_TIMEOUT; i++)
  {
    rc = az_mqtt_transport_connect_poll(f->t, 0);
  }
  return rc;
}

/**
 * @brief Connect: the request goes out, then @p reply_prefix + Accept + @p reply_suffix and
 * @p after (server frames) come back @p chunk bytes at a time.
 */
static az_result _connect(
    fixture* f,
    char const* reply_prefix,
    char const* reply_suffix,
    int32_t chunk,
    void (*after)(fixture*))
{
  assert_int_equal(az_mqtt_transport_connect_start(f->t, AZ_SPAN_FROM_STR("h"), 80, NULL), AZ_OK);
  f->fake.connect_polls_pending = 2;
  assert_int_equal(az_mqtt_transport_connect_poll(f->t, 0), AZ_MQTT_ERROR_TIMEOUT);
  assert_int_equal(f->fake.send_calls, 0); // Not before the lower transport is up.
  assert_int_equal(az_mqtt_transport_connect_poll(f->t, 0), AZ_MQTT_ERROR_TIMEOUT);
  assert_int_equal(az_mqtt_transport_connect_poll(f->t, 0), AZ_MQTT_ERROR_TIMEOUT);
  assert_int_equal(f->fake.send_calls, 1); // The request; no reply yet.
  s_sent_read = f->fake.sent_size;
  char accept[29];
  _accept_for_request(accept);
  test_fake_transport_feed(&f->fake, reply_prefix, (int32_t)strlen(reply_prefix));
  test_fake_transport_feed(&f->fake, accept, 28);
  test_fake_transport_feed(&f->fake, reply_suffix, (int32_t)strlen(reply_suffix));
  if (after != NULL)
  {
    after(f);
  }
  f->fake.chunk = chunk;
  return _poll(f);
}

#define K_REPLY_PREFIX                                                                     \
  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"     \
  "Sec-WebSocket-Accept: "
#define K_REPLY_SUFFIX "\r\nSec-WebSocket-Protocol: mqtt\r\n\r\n"

/** @brief Decode the next client frame sent: checks FIN, mask and minimal length. */
static uint8_t _next_sent_frame(fixture* f, uint8_t* payload, int32_t* out_size)
{
  uint8_t const* p = s_sent + s_sent_read;
  assert_true(f->fake.sent_size - s_sent_read >= 6);
  assert_int_equal(p[0] & 0xF0, 0x80); // FIN, no RSV.
  assert_int_equal(p[1] & 0x80, 0x80); // Masked.
  uint64_t size = p[1] & 0x7F;
  int32_t n = 2;
  if (size == 126)
  {
    size = (uint64_t)p[2] << 8 | p[3];
    assert_true(size >= 126);
    n = 4;
  }
  else if (size == 127)
  {
    size = 0;
    for (int i = 0; i < 8; i++)
    {
      size = size << 8 | p[2 + i];
    }
    assert_true(size > 65535);
    n = 10;
  }
  uint8_t const* mask = p + n;
  for (uint64_t i = 0; i < size; i++)
  {
    payload[i] = p[n + 4 + i] ^ mask[i & 3];
  }
  s_sent_read += n + 4 + (int32_t)size;
  *out_size = (int32_t)size;
  return p[0] & 0x0F;
}

static void _ping_and_data(fixture* f)
{
  _feed_frame(f, 0x82, "ab", 2);
  _feed_frame(f, 0x89, "p", 1);
  _feed_frame(f, 0x02, "c", 1); // Fragmented: continued below.
  _feed_frame(f, 0x8A, "", 0);
  _feed_frame(f, 0x80, "de", 2);
}

/** @brief Receive until @p want payload bytes arrived, through a buffer of @p buffer_size. */
static void _receive_all(fixture* f, char const* want, int32_t buffer_size)
{
  uint8_t got[64];
  int32_t size = 0;
  int32_t const want_size = (int32_t)strlen(want);
  for (int i = 0; i < 1000 && size < want_size; i++)
  {
    uint8_t buffer[64];
    az_span received;
    assert_int_equal(
        az_mqtt_transport_receive(f->t, az_span_create(buffer, buffer_size), 0, &received), AZ_OK);
    assert_ptr_equal(az_span_ptr(received), buffer);
    memcpy(got + size, buffer, (size_t)az_span_size(received));
    size += az_span_size(received);
  }
  assert_int_equal(size, want_size);
  assert_memory_equal(got, want, (size_t)size);
}

static void the_upgrade_and_frames_work_in_any_pieces(void** state)
{
  (void)state;
  for (int32_t chunk = 1; chunk <= 200; chunk++)
  {
    fixture f;
    _init(&f, NULL);
    assert_int_equal(_connect(&f, K_REPLY_PREFIX, K_REPLY_SUFFIX, chunk, _ping_and_data), AZ_OK);
    assert_non_null(strstr((char const*)s_sent, "GET /mqtt HTTP/1.1\r\nHost: h\r\n"));
    _receive_all(&f, "abcde", chunk % 7 + 1); // Through small buffers too.
    uint8_t pong[8];
    int32_t size;
    assert_int_equal(_next_sent_frame(&f, pong, &size), _AZ_MQTT_WEBSOCKET_PONG);
    assert_int_equal(size, 1);
    assert_int_equal(pong[0], 'p');
    assert_int_equal(s_native.count, 0);
  }
}

static void sends_are_masked_binary_frames(void** state)
{
  (void)state;
  static uint8_t data[70000];
  static uint8_t decoded[70000];
  for (size_t i = 0; i < sizeof(data); i++)
  {
    data[i] = (uint8_t)(i * 31);
  }
  fixture f;
  _init(&f, NULL);
  assert_int_equal(_connect(&f, K_REPLY_PREFIX, K_REPLY_SUFFIX, 1000, NULL), AZ_OK);
  int32_t const sizes[] = { 0, 1, 125, 126, AZ_MQTT_WEBSOCKET_SEND_CHUNK, 600, 65535, 65536, 70000 };
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
  {
    assert_int_equal(az_mqtt_transport_send(f.t, az_span_create(data, sizes[i])), AZ_OK);
    int32_t size;
    assert_int_equal(_next_sent_frame(&f, decoded, &size), _AZ_MQTT_WEBSOCKET_BINARY);
    assert_int_equal(size, sizes[i]);
    assert_memory_equal(decoded, data, (size_t)size);
  }
  assert_int_equal(s_sent_read, f.fake.sent_size);
}

static void a_send_failure_is_returned(void** state)
{
  (void)state;
  static uint8_t data[AZ_MQTT_WEBSOCKET_SEND_CHUNK * 2];
  fixture f;
  _init(&f, NULL);
  assert_int_equal(_connect(&f, K_REPLY_PREFIX, K_REPLY_SUFFIX, 1000, NULL), AZ_OK);
  f.fake.fail_send_call = f.fake.send_calls + 2; // The frame's second piece.
  assert_int_equal(
      az_mqtt_transport_send(f.t, AZ_SPAN_FROM_BUFFER(data)), AZ_MQTT_ERROR_CONNECTION_CLOSED);
  // A partial frame leaves the stream unusable: nothing more is sent.
  f.fake.fail_send_call = 0;
  int const calls = f.fake.send_calls;
  assert_int_equal(az_mqtt_transport_send(f.t, AZ_SPAN_FROM_STR("x")), AZ_MQTT_ERROR_NOT_CONNECTED);
  assert_int_equal(f.fake.send_calls, calls);
}

static void a_refused_upgrade_fails_and_closes(void** state)
{
  (void)state;
  struct
  {
    char const* prefix;
    char const* suffix;
    int32_t status;
  } const cases[] = {
    { "HTTP/1.1 403 Forbidden\r\nX-Accept: ", "\r\n\r\n", 403 },
    { "HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nX-Accept: ", "\r\n\r\n", 101 },
    { "SSH-2.0 ", "\r\n\r\n", 0 },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    fixture f;
    _init(&f, NULL);
    assert_int_equal(_connect(&f, cases[i].prefix, cases[i].suffix, 5, NULL), AZ_MQTT_ERROR_WEBSOCKET);
    assert_int_equal(f.fake.closes, 1);
    assert_int_equal(s_native.count, 1);
    assert_int_equal(s_native.errors[0].source, AZ_MQTT_NATIVE_ERROR_WEBSOCKET);
    assert_int_equal(s_native.errors[0].code, cases[i].status);
    assert_int_equal(s_native.errors[0].result, AZ_MQTT_ERROR_WEBSOCKET);
    assert_int_equal(s_native.errors[0].connect_attempt, 1);
    az_span received;
    uint8_t buffer[8];
    assert_int_equal(
        az_mqtt_transport_receive(f.t, AZ_SPAN_FROM_BUFFER(buffer), 0, &received),
        AZ_MQTT_ERROR_NOT_CONNECTED);
  }
}

static void a_lower_failure_ends_the_upgrade(void** state)
{
  (void)state;
  fixture f;
  _init(&f, NULL);
  assert_int_equal(az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("h"), 80, NULL), AZ_OK);
  f.fake.end_of_input = AZ_MQTT_ERROR_CONNECTION_CLOSED;
  assert_int_equal(_poll(&f), AZ_MQTT_ERROR_CONNECTION_CLOSED);
  assert_int_equal(f.fake.closes, 1);
}

static void _close_1001(fixture* f) { _feed_frame(f, 0x88, "\x03\xe9" "bye", 5); }

static void a_server_close_is_echoed_and_reported(void** state)
{
  (void)state;
  fixture f;
  _init(&f, NULL);
  assert_int_equal(_connect(&f, K_REPLY_PREFIX, K_REPLY_SUFFIX, 3, _close_1001), AZ_OK);
  az_result rc = AZ_OK;
  for (int i = 0; i < 100 && rc == AZ_OK; i++)
  {
    uint8_t buffer[16];
    az_span received;
    rc = az_mqtt_transport_receive(f.t, AZ_SPAN_FROM_BUFFER(buffer), 0, &received);
  }
  assert_int_equal(rc, AZ_MQTT_ERROR_CONNECTION_CLOSED);
  uint8_t body[8];
  int32_t size;
  assert_int_equal(_next_sent_frame(&f, body, &size), _AZ_MQTT_WEBSOCKET_CLOSE);
  assert_int_equal(size, 2);
  assert_int_equal(body[0] << 8 | body[1], 1001);
  assert_int_equal(s_native.count, 1);
  assert_int_equal(s_native.errors[0].code, 1001);
  assert_int_equal(s_native.errors[0].result, AZ_MQTT_ERROR_CONNECTION_CLOSED);
  // Closed by the server: no close frame of our own.
  int32_t const sent = f.fake.sent_size;
  az_mqtt_transport_shutdown(f.t);
  az_mqtt_transport_close(f.t);
  assert_int_equal(f.fake.sent_size, sent);
  assert_int_equal(f.fake.closes, 1);
}

static void _data_then_close(fixture* f)
{
  _feed_frame(f, 0x82, "ab", 2);
  _feed_frame(f, 0x88, "\x03\xe8", 2);
}

static void _data_then_text_frame(fixture* f)
{
  _feed_frame(f, 0x82, "ab", 2);
  _feed_frame(f, 0x81, "x", 1);
}

static void payload_before_a_close_or_bad_frame_is_delivered_first(void** state)
{
  (void)state;
  void (*const streams[])(fixture*) = { _data_then_close, _data_then_text_frame };
  az_result const ends[] = { AZ_MQTT_ERROR_CONNECTION_CLOSED, AZ_MQTT_ERROR_WEBSOCKET };
  for (int i = 0; i < 2; i++)
  {
    // with_reply: the frames arrive in the same read as the upgrade reply; else in a later one.
    for (int with_reply = 0; with_reply < 2; with_reply++)
    {
      fixture f;
      _init(&f, NULL);
      assert_int_equal(
          _connect(&f, K_REPLY_PREFIX, K_REPLY_SUFFIX, 4096, with_reply ? streams[i] : NULL), AZ_OK);
      if (!with_reply)
      {
        streams[i](&f);
      }
      uint8_t buffer[16];
      az_span received;
      assert_int_equal(
          az_mqtt_transport_receive(f.t, AZ_SPAN_FROM_BUFFER(buffer), 0, &received), AZ_OK);
      assert_int_equal(az_span_size(received), 2);
      assert_memory_equal(buffer, "ab", 2);
      for (int k = 0; k < 2; k++) // And after.
      {
        assert_int_equal(
            az_mqtt_transport_receive(f.t, AZ_SPAN_FROM_BUFFER(buffer), 0, &received), ends[i]);
        assert_int_equal(az_span_size(received), 0);
      }
      assert_int_equal(s_native.count, i == 0 ? 0 : 1); // Close 1000 is not an error.
      az_mqtt_transport_close(f.t);
    }
  }
}

static void _text_frame(fixture* f) { _feed_frame(f, 0x81, "x", 1); }

static void a_forbidden_frame_fails_with_1002(void** state)
{
  (void)state;
  fixture f;
  _init(&f, NULL);
  assert_int_equal(_connect(&f, K_REPLY_PREFIX, K_REPLY_SUFFIX, 64, _text_frame), AZ_OK);
  uint8_t buffer[16];
  az_span received;
  assert_int_equal(
      az_mqtt_transport_receive(f.t, AZ_SPAN_FROM_BUFFER(buffer), 0, &received),
      AZ_MQTT_ERROR_WEBSOCKET);
  assert_int_equal(s_native.count, 1);
  assert_int_equal(s_native.errors[0].code, 1002);
  int32_t const sent = f.fake.sent_size;
  az_mqtt_transport_shutdown(f.t);
  assert_int_equal(f.fake.sent_size, sent);
}

static void shutdown_sends_close_1000_once(void** state)
{
  (void)state;
  fixture f;
  _init(&f, NULL);
  assert_int_equal(_connect(&f, K_REPLY_PREFIX, K_REPLY_SUFFIX, 64, NULL), AZ_OK);
  az_mqtt_transport_shutdown(f.t);
  az_mqtt_transport_shutdown(f.t);
  assert_int_equal(f.fake.shutdowns, 2); // Forwarded.
  uint8_t body[8];
  int32_t size;
  assert_int_equal(_next_sent_frame(&f, body, &size), _AZ_MQTT_WEBSOCKET_CLOSE);
  assert_int_equal(size, 2);
  assert_int_equal(body[0] << 8 | body[1], 1000);
  assert_int_equal(s_sent_read, f.fake.sent_size);
  uint8_t data[1] = { 0 };
  assert_int_equal(az_mqtt_transport_send(f.t, AZ_SPAN_FROM_BUFFER(data)), AZ_MQTT_ERROR_NOT_CONNECTED);
  az_mqtt_transport_close(f.t);
  assert_int_equal(f.fake.closes, 1);
}

static void each_connect_upgrades_afresh(void** state)
{
  (void)state;
  fixture f;
  _init(&f, NULL);
  assert_int_equal(_connect(&f, K_REPLY_PREFIX, K_REPLY_SUFFIX, 64, NULL), AZ_OK);
  az_mqtt_transport_close(f.t);
  memset(s_sent, 0, sizeof(s_sent));
  f.fake.sent_size = 0;
  f.fake.send_calls = 0;
  s_sent_read = 0;
  assert_int_equal(_connect(&f, K_REPLY_PREFIX, K_REPLY_SUFFIX, 64, NULL), AZ_OK);
  assert_int_equal(f.fake.connects, 2);
  // Errors of the second connect carry its attempt number.
  _feed_frame(&f, 0x81, "x", 1);
  uint8_t buffer[8];
  az_span received;
  assert_int_equal(
      az_mqtt_transport_receive(f.t, AZ_SPAN_FROM_BUFFER(buffer), 0, &received),
      AZ_MQTT_ERROR_WEBSOCKET);
  assert_int_equal(s_native.errors[0].connect_attempt, 2);
}

static void the_request_uses_the_path_host_and_port(void** state)
{
  (void)state;
  az_mqtt_websocket_options options = az_mqtt_websocket_options_default();
  options.path = AZ_SPAN_FROM_STR(AZ_MQTT_WEBSOCKET_PATH_IOT_HUB);
  az_mqtt_tls_options tls = az_mqtt_tls_options_default();
  struct
  {
    uint16_t port;
    az_mqtt_tls_options const* tls;
    char const* host_line;
  } const cases[] = {
    { 443, &tls, "\r\nHost: hub\r\n" },
    { 80, &tls, "\r\nHost: hub:80\r\n" },
    { 8080, NULL, "\r\nHost: hub:8080\r\n" },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    fixture f;
    _init(&f, &options);
    assert_int_equal(
        az_mqtt_transport_connect_start(f.t, AZ_SPAN_FROM_STR("hub"), cases[i].port, cases[i].tls),
        AZ_OK);
    assert_ptr_equal(f.fake.tls_options, cases[i].tls); // TLS is the lower transport's.
    assert_int_equal(az_mqtt_transport_connect_poll(f.t, 0), AZ_MQTT_ERROR_TIMEOUT);
    assert_non_null(strstr((char const*)s_sent, "GET /$iothub/websocket HTTP/1.1\r\n"));
    assert_non_null(strstr((char const*)s_sent, cases[i].host_line));
  }
}

static void settings_reach_the_lower_transport(void** state)
{
  (void)state;
  fixture f;
  _init(&f, NULL);
  assert_ptr_equal(f.fake.error_callback, _on_native);
  az_mqtt_proxy_options proxy;
  memset(&proxy, 0, sizeof(proxy));
  proxy.host = AZ_SPAN_FROM_STR("proxy");
  proxy.port = 3128;
  assert_int_equal(az_mqtt_transport_set_proxy(f.t, &proxy), AZ_OK);
  assert_ptr_equal(f.fake.proxy, &proxy);

  az_mqtt_websocket ws;
  az_mqtt_websocket_options options = az_mqtt_websocket_options_default();
  options.path = AZ_SPAN_FROM_STR("mqtt");
  assert_int_equal(az_mqtt_websocket_init(&ws, &f.fake.layer.base, &options), AZ_MQTT_ERROR_INVALID_CONFIG);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(the_upgrade_and_frames_work_in_any_pieces),
    cmocka_unit_test(sends_are_masked_binary_frames),
    cmocka_unit_test(a_send_failure_is_returned),
    cmocka_unit_test(a_refused_upgrade_fails_and_closes),
    cmocka_unit_test(a_lower_failure_ends_the_upgrade),
    cmocka_unit_test(a_server_close_is_echoed_and_reported),
    cmocka_unit_test(a_forbidden_frame_fails_with_1002),
    cmocka_unit_test(payload_before_a_close_or_bad_frame_is_delivered_first),
    cmocka_unit_test(shutdown_sends_close_1000_once),
    cmocka_unit_test(each_connect_upgrades_afresh),
    cmocka_unit_test(the_request_uses_the_path_host_and_port),
    cmocka_unit_test(settings_reach_the_lower_transport),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
