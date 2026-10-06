// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

#include <cmocka.h>

#include "az_mqtt_http_connect.h"

static az_span _str(char const* s)
{
  return az_span_create((uint8_t*)(uintptr_t)s, (int32_t)strlen(s));
}

static az_mqtt_proxy_options _proxy(char const* user, char const* password)
{
  az_mqtt_proxy_options p;
  memset(&p, 0, sizeof(p));
  p.host = _str("proxy.example");
  p.port = 3128;
  p.username = _str(user);
  p.password = _str(password);
  return p;
}

/** @brief The request for @p host:@p port through @p proxy, as a C string. */
static char const* _request(az_mqtt_proxy_options const* proxy, char const* host, uint16_t port)
{
  static char text[_AZ_MQTT_HTTP_CONNECT_REQUEST_MAX + 1];
  int32_t size = 0;
  az_span const buffer = az_span_create((uint8_t*)text, (int32_t)sizeof(text) - 1);
  assert_int_equal(_az_mqtt_http_connect_request(proxy, _str(host), port, buffer, &size), AZ_OK);
  text[size] = '\0';
  return text;
}

static void a_request_names_the_target_not_the_proxy(void** state)
{
  (void)state;
  az_mqtt_proxy_options const p = _proxy("", "");
  assert_string_equal(
      _request(&p, "hub.example", 8883),
      "CONNECT hub.example:8883 HTTP/1.1\r\nHost: hub.example:8883\r\n\r\n");
  assert_string_equal(
      _request(&p, "::1", 1883), "CONNECT [::1]:1883 HTTP/1.1\r\nHost: [::1]:1883\r\n\r\n");
}

static void credentials_are_sent_verbatim_as_basic(void** state)
{
  (void)state;
  // Delimiters and percent signs are not decoded or escaped: base64("dev@corp:p@ss%77rd:x").
  az_mqtt_proxy_options const p = _proxy("dev@corp", "p@ss%77rd:x");
  assert_string_equal(
      _request(&p, "hub.example", 8883),
      "CONNECT hub.example:8883 HTTP/1.1\r\nHost: hub.example:8883\r\n"
      "Proxy-Authorization: Basic ZGV2QGNvcnA6cEBzcyU3N3JkOng=\r\n\r\n");
}

static void the_longest_request_fits(void** state)
{
  (void)state;
  char host[AZ_MQTT_PROXY_HOST_MAX + 1];
  memset(host, 'h', AZ_MQTT_PROXY_HOST_MAX);
  host[AZ_MQTT_PROXY_HOST_MAX] = '\0';
  char user[AZ_MQTT_PROXY_CREDENTIALS_MAX];
  memset(user, 'u', AZ_MQTT_PROXY_CREDENTIALS_MAX - 1);
  user[AZ_MQTT_PROXY_CREDENTIALS_MAX - 1] = '\0';
  az_mqtt_proxy_options p = _proxy(user, "p");
  p.host = _str(host);
  assert_int_equal(_az_mqtt_http_connect_check(&p), AZ_OK);
  (void)_request(&p, host, 65535);
}

static void invalid_options_are_refused(void** state)
{
  (void)state;
  assert_int_equal(_az_mqtt_http_connect_check(NULL), AZ_OK);
  az_mqtt_proxy_options p = _proxy("", "");
  p.host = AZ_SPAN_EMPTY; // No proxy.
  assert_int_equal(_az_mqtt_http_connect_check(&p), AZ_OK);

  char const* const bad_users[] = { "a\r\nX-Injected: 1", "a:b", "a\nb" };
  for (size_t i = 0; i < sizeof(bad_users) / sizeof(bad_users[0]); i++)
  {
    p = _proxy(bad_users[i], "pw");
    assert_int_equal(_az_mqtt_http_connect_check(&p), AZ_MQTT_ERROR_INVALID_CONFIG);
  }
  p = _proxy("u", "p\r\nX: 1");
  assert_int_equal(_az_mqtt_http_connect_check(&p), AZ_MQTT_ERROR_INVALID_CONFIG);
  p = _proxy("", "password-without-user");
  assert_int_equal(_az_mqtt_http_connect_check(&p), AZ_MQTT_ERROR_INVALID_CONFIG);
  p = _proxy("", "");
  p.port = 0;
  assert_int_equal(_az_mqtt_http_connect_check(&p), AZ_MQTT_ERROR_INVALID_CONFIG);
  p = _proxy("", "");
  p.host = _str("proxy\r\n");
  assert_int_equal(_az_mqtt_http_connect_check(&p), AZ_MQTT_ERROR_INVALID_CONFIG);
  char long_user[AZ_MQTT_PROXY_CREDENTIALS_MAX + 1];
  memset(long_user, 'u', AZ_MQTT_PROXY_CREDENTIALS_MAX);
  long_user[AZ_MQTT_PROXY_CREDENTIALS_MAX] = '\0';
  p = _proxy(long_user, "p");
  assert_int_equal(_az_mqtt_http_connect_check(&p), AZ_MQTT_ERROR_INVALID_CONFIG);
  p = _proxy("u", long_user);
  assert_int_equal(_az_mqtt_http_connect_check(&p), AZ_MQTT_ERROR_INVALID_CONFIG);
#if defined(__unix__) || defined(__APPLE__)
  // Sizes whose sum overflows int32_t: refused (readable, so a check that reads stays in bounds).
  void* const zeros
      = mmap(NULL, (size_t)INT32_MAX, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (zeros != MAP_FAILED)
  {
    p = _proxy("", "");
    p.username = az_span_create((uint8_t*)zeros, INT32_MAX);
    p.password = az_span_create((uint8_t*)zeros, INT32_MAX);
    assert_int_equal(_az_mqtt_http_connect_check(&p), AZ_MQTT_ERROR_INVALID_CONFIG);
    (void)munmap(zeros, (size_t)INT32_MAX);
  }
#endif

  // The target host goes in the request line too.
  p = _proxy("", "");
  uint8_t buf[_AZ_MQTT_HTTP_CONNECT_REQUEST_MAX];
  int32_t size = 0;
  assert_int_equal(
      _az_mqtt_http_connect_request(&p, _str("hub\r\nX: 1"), 1, AZ_SPAN_FROM_BUFFER(buf), &size),
      AZ_MQTT_ERROR_INVALID_CONFIG);
  assert_int_equal(
      _az_mqtt_http_connect_request(&p, _str("hub x"), 1, AZ_SPAN_FROM_BUFFER(buf), &size),
      AZ_MQTT_ERROR_INVALID_CONFIG);
  // Other controls (HTAB, DEL) would change tokenization too.
  char const* const controls[] = { "hub	x", "hub", "hub" };
  for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++)
  {
    assert_int_equal(
        _az_mqtt_http_connect_request(&p, _str(controls[i]), 1, AZ_SPAN_FROM_BUFFER(buf), &size),
        AZ_MQTT_ERROR_INVALID_CONFIG);
    p.host = _str(controls[i]);
    assert_int_equal(_az_mqtt_http_connect_check(&p), AZ_MQTT_ERROR_INVALID_CONFIG);
    p = _proxy("", "");
  }
}

/** @brief Parse @p reply in pieces of @p piece bytes; *out_consumed sums what was consumed. */
static az_result _parse(
    char const* reply,
    int32_t piece,
    int32_t* out_consumed,
    uint16_t* out_status)
{
  _az_mqtt_http_reply r;
  _az_mqtt_http_reply_init(&r);
  int32_t const size = (int32_t)strlen(reply);
  int32_t offset = 0;
  az_result rc = AZ_MQTT_ERROR_TIMEOUT;
  while (rc == AZ_MQTT_ERROR_TIMEOUT && offset < size)
  {
    int32_t const n = size - offset < piece ? size - offset : piece;
    int32_t consumed = -1;
    rc = _az_mqtt_http_connect_reply_parse(
        &r, az_span_slice(_str(reply), offset, offset + n), &consumed);
    assert_true(consumed >= 0 && consumed <= n);
    offset += consumed;
  }
  *out_consumed = offset;
  *out_status = r.status;
  return rc;
}

static void a_2xx_reply_opens_the_tunnel_in_any_number_of_pieces(void** state)
{
  (void)state;
  char const* const replies[] = {
    "HTTP/1.1 200 Connection established\r\n\r\n",
    "HTTP/1.0 200 OK\r\nProxy-Agent: test\r\nVia: 1.1 proxy\r\n\r\n",
    "HTTP/1.1 200\r\n\r\n",
    "HTTP/1.1 204 No Content\n\n",
    "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); i++)
  {
    int32_t const size = (int32_t)strlen(replies[i]);
    for (int32_t piece = 1; piece <= size; piece++)
    {
      int32_t consumed = 0;
      uint16_t status = 0;
      assert_int_equal(_parse(replies[i], piece, &consumed, &status), AZ_OK);
      assert_int_equal(consumed, size);
      assert_true(status >= 200 && status < 300);
    }
  }
}

static void bytes_after_the_reply_are_not_consumed(void** state)
{
  (void)state;
  char const reply[] = "HTTP/1.1 200 OK\r\n\r\n\x16\x03\x01";
  int32_t consumed = 0;
  uint16_t status = 0;
  assert_int_equal(_parse(reply, (int32_t)strlen(reply), &consumed, &status), AZ_OK);
  assert_int_equal(consumed, (int32_t)strlen(reply) - 3);
}

static void long_headers_are_skipped(void** state)
{
  (void)state;
  static char reply[_AZ_MQTT_HTTP_REPLY_MAX];
  static char const status_line[] = "HTTP/1.1 200 OK\r\n";
  memcpy(reply, status_line, sizeof(status_line) - 1);
  size_t len = sizeof(status_line) - 1;
  while (len + 100 < sizeof(reply) - 64)
  {
    memcpy(reply + len, "X-Padding: ", 11);
    memset(reply + len + 11, 'x', 85);
    memcpy(reply + len + 96, "\r\n", 2);
    len += 98;
  }
  memcpy(reply + len, "\r\n", 3);
  int32_t consumed = 0;
  uint16_t status = 0;
  assert_int_equal(_parse(reply, 1000, &consumed, &status), AZ_OK);
}

static void refusals_and_malformed_replies_fail(void** state)
{
  (void)state;
  struct
  {
    char const* reply;
    az_result rc;
    uint16_t status;
  } const cases[] = {
    { "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic\r\n\r\n",
      AZ_MQTT_ERROR_PROXY_AUTH,
      407 },
    { "HTTP/1.1 403 Forbidden\r\n\r\n", AZ_MQTT_ERROR_PROXY, 403 },
    { "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n", AZ_MQTT_ERROR_PROXY, 502 },
    { "SSH-2.0-OpenSSH\r\n", AZ_MQTT_ERROR_PROXY, 0 },
    { "HTTP/1.1 20 OK\r\n\r\n", AZ_MQTT_ERROR_PROXY, 0 },
    { "HTTP/1.1 2000 OK\r\n\r\n", AZ_MQTT_ERROR_PROXY, 0 },
    { "HTTP/2 200\r\n\r\n", AZ_MQTT_ERROR_PROXY, 0 },
    { "HTTP/1.1 200 OK\r\n\rX", AZ_MQTT_ERROR_PROXY, 0 },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    for (int32_t piece = 1; piece <= 3; piece++)
    {
      int32_t consumed = 0;
      uint16_t status = 0;
      assert_int_equal(_parse(cases[i].reply, piece, &consumed, &status), cases[i].rc);
      assert_int_equal(status, cases[i].status);
    }
  }
}

static void control_bytes_in_the_reply_are_refused(void** state)
{
  (void)state;
  static struct
  {
    char const bytes[40];
    int32_t size;
  } const cases[] = {
    { "HTTP/1.1 200 OK\0\r\n\r\n", 20 }, // NUL in the reason phrase
    { "HTTP/1.1 200 OK\r\nX: a\0b\r\n\r\n", 27 }, // NUL in a header
    { "HTTP/1.1 200 OK\r\nX: a\x01\r\n\r\n", 26 }, // other control byte
    { "HTTP/1.1 200 OK\r\nX: a\x7f\r\n\r\n", 26 }, // DEL
    { "HTTP/1.1 200 OK\rX: a\r\n\r\n", 24 }, // bare CR ending a line
    { "HTTP/1.1 200\rOK\r\n\r\n", 19 }, // bare CR after the status
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    for (int32_t piece = 1; piece <= cases[i].size; piece++)
    {
      _az_mqtt_http_reply r;
      _az_mqtt_http_reply_init(&r);
      az_span const all = az_span_create((uint8_t*)(uintptr_t)cases[i].bytes, cases[i].size);
      az_result rc = AZ_MQTT_ERROR_TIMEOUT;
      for (int32_t offset = 0; rc == AZ_MQTT_ERROR_TIMEOUT && offset < cases[i].size;)
      {
        int32_t const end = offset + piece < cases[i].size ? offset + piece : cases[i].size;
        int32_t consumed = 0;
        rc = _az_mqtt_http_connect_reply_parse(&r, az_span_slice(all, offset, end), &consumed);
        offset += consumed;
      }
      assert_int_equal(rc, AZ_MQTT_ERROR_PROXY);
    }
  }
  // Tabs and obs-text are allowed.
  char const ok[] = "HTTP/1.1 200 \xc3\xa9t\xc3\xa9\r\nX:\ta\tb\r\n\r\n";
  _az_mqtt_http_reply r;
  _az_mqtt_http_reply_init(&r);
  int32_t consumed = 0;
  assert_int_equal(
      _az_mqtt_http_connect_reply_parse(
          &r, az_span_create((uint8_t*)(uintptr_t)ok, (int32_t)sizeof(ok) - 1), &consumed),
      AZ_OK);
}

/** @brief A send that takes at most `chunk` bytes per call, says "would block" every
 * `block_every` calls, and fails after `fail_after` bytes (-1: never). */
typedef struct
{
  char out[_AZ_MQTT_HTTP_CONNECT_REQUEST_MAX + 1];
  int32_t size;
  int32_t chunk;
  int calls;
  int block_every;
  int32_t fail_after;
} fake_socket;

static int32_t _fake_send(void* context, uint8_t const* data, int32_t size)
{
  fake_socket* const s = (fake_socket*)context;
  s->calls++;
  if (s->block_every > 0 && s->calls % s->block_every == 0)
  {
    return 0;
  }
  if (s->fail_after >= 0 && s->size >= s->fail_after)
  {
    return -1;
  }
  int32_t const n = size < s->chunk ? size : s->chunk;
  if (s->size + n > _AZ_MQTT_HTTP_CONNECT_REQUEST_MAX)
  {
    return -1; // More than any request: bytes were sent twice.
  }
  memcpy(s->out + s->size, data, (size_t)n);
  s->size += n;
  return n;
}

static void the_request_is_sent_across_partial_and_blocked_sends(void** state)
{
  (void)state;
  az_mqtt_proxy_options const p = _proxy("dev@corp", "p@ss%77rd:x");
  char const* const expected = _request(&p, "hub.example", 8883);
  for (int32_t chunk = 1; chunk <= 7; chunk += 3)
  {
    for (int block_every = 0; block_every <= 3; block_every += 3)
    {
      fake_socket s;
      memset(&s, 0, sizeof(s));
      s.chunk = chunk;
      s.block_every = block_every;
      s.fail_after = -1;
      int32_t sent = 0;
      int blocked = 0;
      az_result rc = AZ_MQTT_ERROR_TIMEOUT;
      for (int i = 0; i < 4000 && rc == AZ_MQTT_ERROR_TIMEOUT; i++)
      {
        int32_t const before = sent;
        rc = _az_mqtt_http_connect_send_request(
            &p, _str("hub.example"), 8883, &sent, _fake_send, &s);
        assert_true(sent >= before); // Progress is kept across calls.
        blocked += rc == AZ_MQTT_ERROR_TIMEOUT;
      }
      assert_int_equal(rc, AZ_OK);
      assert_int_equal(sent, (int32_t)strlen(expected));
      assert_string_equal(s.out, expected);
      assert_true(block_every == 0 ? blocked == 0 : blocked > 0);
      // Done: further calls send nothing more.
      assert_int_equal(
          _az_mqtt_http_connect_send_request(&p, _str("hub.example"), 8883, &sent, _fake_send, &s),
          AZ_OK);
      assert_int_equal(s.size, (int32_t)strlen(expected));
    }
  }
}

static void a_failed_send_is_a_proxy_error(void** state)
{
  (void)state;
  az_mqtt_proxy_options const p = _proxy("", "");
  fake_socket s;
  memset(&s, 0, sizeof(s));
  s.chunk = 5;
  s.fail_after = 10;
  int32_t sent = 0;
  assert_int_equal(
      _az_mqtt_http_connect_send_request(&p, _str("hub.example"), 8883, &sent, _fake_send, &s),
      AZ_MQTT_ERROR_PROXY);
  assert_int_equal(sent, 10);
  assert_int_equal(
      _az_mqtt_http_connect_send_request(&p, _str("bad host"), 1, &sent, _fake_send, &s),
      AZ_MQTT_ERROR_INVALID_CONFIG);
}

static void an_endless_reply_is_cut_off(void** state)
{
  (void)state;
  _az_mqtt_http_reply r;
  _az_mqtt_http_reply_init(&r);
  int32_t consumed = 0;
  assert_int_equal(
      _az_mqtt_http_connect_reply_parse(&r, _str("HTTP/1.1 200 OK\r\nX: "), &consumed),
      AZ_MQTT_ERROR_TIMEOUT);
  char chunk[256];
  memset(chunk, 'x', sizeof(chunk));
  az_result rc = AZ_MQTT_ERROR_TIMEOUT;
  for (int i = 0; i < 64 && rc == AZ_MQTT_ERROR_TIMEOUT; i++)
  {
    rc = _az_mqtt_http_connect_reply_parse(
        &r, az_span_create((uint8_t*)chunk, (int32_t)sizeof(chunk)), &consumed);
  }
  assert_int_equal(rc, AZ_MQTT_ERROR_PROXY);
  assert_int_equal(r.status, 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_request_names_the_target_not_the_proxy),
    cmocka_unit_test(credentials_are_sent_verbatim_as_basic),
    cmocka_unit_test(the_longest_request_fits),
    cmocka_unit_test(invalid_options_are_refused),
    cmocka_unit_test(a_2xx_reply_opens_the_tunnel_in_any_number_of_pieces),
    cmocka_unit_test(bytes_after_the_reply_are_not_consumed),
    cmocka_unit_test(long_headers_are_skipped),
    cmocka_unit_test(refusals_and_malformed_replies_fail),
    cmocka_unit_test(control_bytes_in_the_reply_are_refused),
    cmocka_unit_test(the_request_is_sent_across_partial_and_blocked_sends),
    cmocka_unit_test(a_failed_send_is_a_proxy_error),
    cmocka_unit_test(an_endless_reply_is_cut_off),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
