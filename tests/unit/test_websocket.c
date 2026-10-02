// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_websocket.c
 * @brief Unit tests: WebSocket handshake and framing (no I/O).
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "az_mqtt_http_connect.h"
#include "az_mqtt_websocket_internal.h"

#include <stdio.h>
#include <string.h>

static az_span _str(char const* s) { return az_span_create((uint8_t*)(uintptr_t)s, (int32_t)strlen(s)); }

static void _hex(uint8_t const digest[20], char out[41])
{
  for (int i = 0; i < 20; i++)
  {
    out[2 * i] = "0123456789abcdef"[digest[i] >> 4];
    out[2 * i + 1] = "0123456789abcdef"[digest[i] & 0xF];
  }
  out[40] = '\0';
}

static void sha1_matches_known_digests(void** state)
{
  (void)state;
  static uint8_t a[1000000];
  memset(a, 'a', sizeof(a));
  struct
  {
    uint8_t const* data;
    size_t size;
    char const* digest;
  } const cases[] = {
    { (uint8_t const*)"", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709" },
    { (uint8_t const*)"abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d" },
    { (uint8_t const*)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
      56,
      "84983e441c3bd26ebaae4aa1f95129e5e54670f1" },
    { a, 55, "c1c8bbdc22796e28c0e15163d20899b65621d65a" },
    { a, 63, "03f09f5b158a7a8cdad920bddc29b81c18a551f5" },
    { a, 64, "0098ba824b5c16427bd7a1122a5a442a25ec644d" },
    { a, 119, "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56" },
    { a, 120, "f34c1488385346a55709ba056ddd08280dd4c6d6" },
    { a, sizeof(a), "34aa973cd4c4daa4f61eeb2bdbad27316534016f" },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    uint8_t digest[20];
    char hex[41];
    _az_mqtt_sha1(cases[i].data, cases[i].size, digest);
    _hex(digest, hex);
    assert_string_equal(hex, cases[i].digest);
  }
}

// RFC 6455 §1.3: nonce "the sample nonce", key dGhlIHNhbXBsZSBub25jZQ==.
static uint8_t const k_nonce[16] = { 't', 'h', 'e', ' ', 's', 'a', 'm', 'p',
                                     'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e' };
#define K_ACCEPT "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

/** @brief Build a request on @p ws; returns it NUL-terminated in @p out. */
static az_result _request(
    az_mqtt_websocket_options* ws,
    char const* host,
    uint16_t port,
    bool tls,
    char* out,
    size_t capacity)
{
  int32_t size = 0;
  az_result rc = _az_mqtt_websocket_request(
      ws, _str(host), port, tls, k_nonce, az_span_create((uint8_t*)out, (int32_t)capacity - 1), &size);
  out[az_result_succeeded(rc) ? size : 0] = '\0';
  return rc;
}

static void the_request_follows_rfc_6455(void** state)
{
  (void)state;
  az_mqtt_websocket_options ws = az_mqtt_websocket_options_default();
  char request[_AZ_MQTT_WEBSOCKET_REQUEST_MAX];
  assert_int_equal(_request(&ws, "hub.example", 443, true, request, sizeof(request)), AZ_OK);
  assert_string_equal(
      request,
      "GET /mqtt HTTP/1.1\r\nHost: hub.example\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Protocol: mqtt\r\n\r\n");

  // Port in Host unless the scheme's default; IPv6 literals bracketed; the given path.
  ws.path = AZ_SPAN_FROM_STR(AZ_MQTT_WEBSOCKET_PATH_IOT_HUB);
  assert_int_equal(_request(&ws, "::1", 8080, false, request, sizeof(request)), AZ_OK);
  assert_non_null(strstr(request, "GET /$iothub/websocket HTTP/1.1\r\nHost: [::1]:8080\r\n"));
  assert_int_equal(_request(&ws, "h", 80, false, request, sizeof(request)), AZ_OK);
  assert_non_null(strstr(request, "\r\nHost: h\r\n"));
  assert_int_equal(_request(&ws, "h", 443, false, request, sizeof(request)), AZ_OK);
  assert_non_null(strstr(request, "\r\nHost: h:443\r\n"));
  assert_int_equal(_request(&ws, "h", 80, true, request, sizeof(request)), AZ_OK);
  assert_non_null(strstr(request, "\r\nHost: h:80\r\n"));
}

static void the_longest_request_fits(void** state)
{
  (void)state;
  static char path[AZ_MQTT_WEBSOCKET_PATH_MAX + 1];
  static char host[256];
  memset(path, 'p', AZ_MQTT_WEBSOCKET_PATH_MAX);
  path[0] = '/';
  memset(host, ':', 255); // An "IPv6 literal": bracketed.
  az_mqtt_websocket_options ws = az_mqtt_websocket_options_default();
  ws.path = _str(path);
  char request[_AZ_MQTT_WEBSOCKET_REQUEST_MAX + 1];
  assert_int_equal(_request(&ws, host, 65535, true, request, sizeof(request)), AZ_OK);
}

static void bad_hosts_and_paths_are_refused(void** state)
{
  (void)state;
  az_mqtt_websocket_options ws = az_mqtt_websocket_options_default();
  char request[_AZ_MQTT_WEBSOCKET_REQUEST_MAX];
  char const* hosts[] = { "", "a b", "a\r\nX: y", "a\n" };
  for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++)
  {
    assert_int_equal(
        _request(&ws, hosts[i], 80, false, request, sizeof(request)),
        AZ_MQTT_ERROR_INVALID_CONFIG);
  }
  static char long_host[257];
  memset(long_host, 'h', 256);
  assert_int_equal(
      _request(&ws, long_host, 80, false, request, sizeof(request)), AZ_MQTT_ERROR_INVALID_CONFIG);

  assert_int_equal(_az_mqtt_websocket_check(NULL), AZ_OK);
  char const* good[] = { "", "/", "/mqtt", AZ_MQTT_WEBSOCKET_PATH_IOT_HUB, "/a?b=c&d=%20~" };
  for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++)
  {
    ws.path = _str(good[i]);
    assert_int_equal(_az_mqtt_websocket_check(&ws), AZ_OK);
  }
  char const* bad[] = { "mqtt", "/a b", "/a\r\nX: y", "/a\tb", "/\x7f", "/\xc3\xa9" };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
  {
    ws.path = _str(bad[i]);
    assert_int_equal(_az_mqtt_websocket_check(&ws), AZ_MQTT_ERROR_INVALID_CONFIG);
  }
  ws.path = az_span_create((uint8_t*)(uintptr_t) "/\0", 2);
  assert_int_equal(_az_mqtt_websocket_check(&ws), AZ_MQTT_ERROR_INVALID_CONFIG);
  static char long_path[AZ_MQTT_WEBSOCKET_PATH_MAX + 2];
  memset(long_path, 'p', AZ_MQTT_WEBSOCKET_PATH_MAX + 1);
  long_path[0] = '/';
  ws.path = _str(long_path);
  assert_int_equal(_az_mqtt_websocket_check(&ws), AZ_MQTT_ERROR_INVALID_CONFIG);
}

/**
 * @brief Request on a fresh @p ws, then parse @p reply in pieces of @p piece bytes.
 * @return The parse result; *out_consumed sums what was consumed.
 */
static az_result _parse(
    az_mqtt_websocket_options* ws,
    char const* reply,
    int32_t size,
    int32_t piece,
    int32_t* out_consumed,
    uint16_t* out_status)
{
  char request[_AZ_MQTT_WEBSOCKET_REQUEST_MAX];
  assert_int_equal(_request(ws, "h", 80, false, request, sizeof(request)), AZ_OK);
  az_result rc = AZ_MQTT_ERROR_TIMEOUT;
  *out_consumed = 0;
  for (int32_t offset = 0; rc == AZ_MQTT_ERROR_TIMEOUT && offset < size; offset += piece)
  {
    int32_t const n = size - offset < piece ? size - offset : piece;
    int32_t consumed = 0;
    rc = _az_mqtt_websocket_reply_parse(
        ws,
        az_span_create((uint8_t*)(uintptr_t)reply + offset, n),
        &consumed,
        out_status);
    *out_consumed += consumed;
  }
  return rc;
}

#define K_GOOD_REPLY                                                                          \
  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"         \
  "Sec-WebSocket-Accept: " K_ACCEPT "\r\nSec-WebSocket-Protocol: mqtt\r\n\r\n"

static void a_valid_reply_upgrades_in_any_number_of_pieces(void** state)
{
  (void)state;
  char const* replies[] = {
    K_GOOD_REPLY,
    // mosquitto's spelling; no protocol; LF line ends; other tokens; spaces around values.
    "HTTP/1.1 101 Switching Protocols\nupgrade:WebSocket\nCONNECTION: keep-alive ,  upgrade\n"
    "Server: x\nsec-websocket-accept:  \t" K_ACCEPT " \n\n",
  };
  for (size_t r = 0; r < sizeof(replies) / sizeof(replies[0]); r++)
  {
    char reply[512];
    int const size = snprintf(reply, sizeof(reply), "%s\x82\x01X", replies[r]);
    for (int32_t piece = 1; piece <= size; piece++)
    {
      az_mqtt_websocket_options ws = az_mqtt_websocket_options_default();
      int32_t consumed;
      uint16_t status;
      assert_int_equal(_parse(&ws, reply, size, piece, &consumed, &status), AZ_OK);
      assert_int_equal(status, 101);
      assert_int_equal(consumed, size - 3); // The frame after it is left alone.
      assert_int_equal(ws._internal.stage, _AZ_MQTT_WEBSOCKET_OPEN);
    }
  }
}

static void a_long_reply_is_accepted(void** state)
{
  (void)state;
  static char reply[_AZ_MQTT_HTTP_REPLY_MAX];
  int len = snprintf(reply, sizeof(reply), "HTTP/1.1 101 OK\r\n");
  while (len < 7000)
  {
    len += snprintf(reply + len, sizeof(reply) - (size_t)len, "X-Padding: %090d\r\n", len);
  }
  // Long header lines that do not matter are fine.
  len += snprintf(
      reply + len,
      sizeof(reply) - (size_t)len,
      "Set-Cookie: %0200d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " K_ACCEPT "\r\n\r\n",
      1);
  az_mqtt_websocket_options ws = az_mqtt_websocket_options_default();
  int32_t consumed;
  uint16_t status;
  assert_int_equal(_parse(&ws, reply, len, 1000, &consumed, &status), AZ_OK);
  assert_int_equal(consumed, len);
}

static void invalid_replies_are_refused(void** state)
{
  (void)state;
  struct
  {
    char const* reply;
    uint16_t status;
  } const cases[] = {
    { "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n", 400 },
    { "HTTP/1.1 404 Not Found\r\n\r\n", 404 },
    { "HTTP/1.1 200 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " K_ACCEPT "\r\n\r\n",
      200 },
    { "HTTP/1.1 101 OK\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " K_ACCEPT "\r\n\r\n", 101 },
    { "HTTP/1.1 101 OK\r\nUpgrade: h2c\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " K_ACCEPT "\r\n\r\n",
      101 },
    { "HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: " K_ACCEPT "\r\n\r\n", 101 },
    { "HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: Upgraded\r\n"
      "Sec-WebSocket-Accept: " K_ACCEPT "\r\n\r\n",
      101 },
    { "HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n", 101 },
    { "HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOO=\r\n\r\n",
      101 },
    { "HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " K_ACCEPT "\r\nSec-WebSocket-Protocol: mqttv5\r\n\r\n",
      101 },
    { "HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " K_ACCEPT "\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n",
      101 },
    // A header the handshake depends on, too long to check whole.
    { "HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " K_ACCEPT "\r\nConnection: a, b, c, d, e, f, g, h, i, j, k, l, m, "
      "n, o, p, q, r, s, t, u, v, w, x, y, z\r\n\r\n",
      101 },
    { "SSH-2.0-OpenSSH\r\n\r\n", 0 },
    { "HTTP/1.1 101 OK\r\nUpgrade: websocket\x01\r\n\r\n", 0 },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    int32_t const size = (int32_t)strlen(cases[i].reply);
    for (int32_t piece = 1; piece <= size; piece += 7)
    {
      az_mqtt_websocket_options ws = az_mqtt_websocket_options_default();
      int32_t consumed;
      uint16_t status;
      assert_int_equal(
          _parse(&ws, cases[i].reply, size, piece, &consumed, &status), AZ_MQTT_ERROR_WEBSOCKET);
      assert_int_equal(status, cases[i].status);
      assert_int_equal(ws._internal.stage, _AZ_MQTT_WEBSOCKET_CLOSED);
    }
  }

  // A reply that never ends.
  static char endless[_AZ_MQTT_HTTP_REPLY_MAX + 64];
  int len = snprintf(endless, sizeof(endless), "HTTP/1.1 101 OK\r\n");
  while (len < (int)sizeof(endless) - 32)
  {
    len += snprintf(endless + len, sizeof(endless) - (size_t)len, "X: y\r\n");
  }
  az_mqtt_websocket_options ws = az_mqtt_websocket_options_default();
  int32_t consumed;
  uint16_t status;
  assert_int_equal(_parse(&ws, endless, len, 100, &consumed, &status), AZ_MQTT_ERROR_WEBSOCKET);
  assert_int_equal(status, 0);
}

static void frame_headers_use_the_shortest_length(void** state)
{
  (void)state;
  uint8_t const mask[4] = { 1, 2, 3, 4 };
  struct
  {
    uint64_t size;
    int32_t header_size;
    uint8_t header[10];
  } const cases[] = {
    { 0, 6, { 0x82, 0x80 } },
    { 125, 6, { 0x82, 0xFD } },
    { 126, 8, { 0x82, 0xFE, 0x00, 0x7E } },
    { 65535, 8, { 0x82, 0xFE, 0xFF, 0xFF } },
    { 65536, 14, { 0x82, 0xFF, 0, 0, 0, 0, 0, 1, 0, 0 } },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    uint8_t out[_AZ_MQTT_WEBSOCKET_HEADER_MAX];
    int32_t const n = _az_mqtt_websocket_frame_header(out, _AZ_MQTT_WEBSOCKET_BINARY, cases[i].size, mask);
    assert_int_equal(n, cases[i].header_size);
    assert_memory_equal(out, cases[i].header, (size_t)n - 4);
    assert_memory_equal(out + n - 4, mask, 4);
  }
}

static void masking_continues_across_pieces(void** state)
{
  (void)state;
  uint8_t const mask[4] = { 0xA1, 0xB2, 0xC3, 0xD4 };
  uint8_t data[11] = "hello world";
  uint8_t whole[11];
  memcpy(whole, data, sizeof(data));
  _az_mqtt_websocket_mask(whole, 11, mask, 0);
  for (int i = 0; i < 11; i++)
  {
    assert_int_equal(whole[i], data[i] ^ mask[i % 4]);
  }
  _az_mqtt_websocket_mask(data, 3, mask, 0);
  _az_mqtt_websocket_mask(data + 3, 8, mask, 3);
  assert_memory_equal(data, whole, sizeof(data));
}

/** @brief Server frame: FIN/opcode byte @p b0, unmasked, @p size payload bytes of @p payload. */
static size_t _frame(uint8_t* out, uint8_t b0, uint8_t const* payload, size_t size)
{
  size_t n = 0;
  out[n++] = b0;
  if (size < 126)
  {
    out[n++] = (uint8_t)size;
  }
  else if (size <= 65535)
  {
    out[n++] = 126;
    out[n++] = (uint8_t)(size >> 8);
    out[n++] = (uint8_t)size;
  }
  else
  {
    out[n++] = 127;
    for (int i = 7; i >= 0; i--)
    {
      out[n++] = (uint8_t)((uint64_t)size >> (8 * i));
    }
  }
  memcpy(out + n, payload, size);
  return n + size;
}

static uint8_t s_stream[80000];
static uint8_t s_expected[80000];

/** @brief Server stream: binary, ping, fragmented binary with a pong between, 126- and 127-length. */
static size_t _stream(size_t* out_expected)
{
  static uint8_t big[70000];
  for (size_t i = 0; i < sizeof(big); i++)
  {
    big[i] = (uint8_t)(i * 13);
  }
  size_t n = 0;
  size_t e = 0;
  n += _frame(s_stream + n, 0x82, (uint8_t const*)"abc", 3);
  n += _frame(s_stream + n, 0x89, (uint8_t const*)"p1", 2);
  n += _frame(s_stream + n, 0x02, (uint8_t const*)"de", 2);
  n += _frame(s_stream + n, 0x8A, (uint8_t const*)"x", 1);
  n += _frame(s_stream + n, 0x00, (uint8_t const*)"", 0);
  n += _frame(s_stream + n, 0x80, (uint8_t const*)"f", 1);
  n += _frame(s_stream + n, 0x82, big, 300);
  n += _frame(s_stream + n, 0x89, (uint8_t const*)"", 0);
  n += _frame(s_stream + n, 0x82, big, sizeof(big));
  memcpy(s_expected + e, "abcdef", 6);
  e += 6;
  memcpy(s_expected + e, big, 300);
  e += 300;
  memcpy(s_expected + e, big, sizeof(big));
  e += sizeof(big);
  *out_expected = e;
  return n;
}

/** @brief Deframe s_stream[0..size) in place, in pieces of @p piece; returns payload size. */
static size_t _deframe_all(az_mqtt_websocket_options* ws, size_t size, size_t piece, int* out_pings)
{
  size_t payload = 0;
  *out_pings = 0;
  for (size_t offset = 0; offset < size; offset += piece)
  {
    int32_t const n = (int32_t)(size - offset < piece ? size - offset : piece);
    int32_t in = 0;
    while (in < n)
    {
      int32_t consumed;
      int32_t produced;
      _az_mqtt_websocket_event const event = _az_mqtt_websocket_deframe(
          ws, s_stream + offset + in, n - in, s_stream + payload, &consumed, &produced);
      in += consumed;
      payload += (size_t)produced;
      if (event == _AZ_MQTT_WEBSOCKET_EVENT_PING)
      {
        az_span const control = _az_mqtt_websocket_control(ws);
        assert_true(
            *out_pings == 0 ? az_span_is_content_equal(control, AZ_SPAN_FROM_STR("p1"))
                            : az_span_size(control) == 0);
        ++*out_pings;
      }
      else
      {
        assert_int_equal(event, _AZ_MQTT_WEBSOCKET_EVENT_DATA);
      }
    }
  }
  return payload;
}

static void frames_deframe_in_place_in_any_pieces(void** state)
{
  (void)state;
  size_t const pieces[] = { 1, 2, 3, 5, 7, 64, 127, 1000, 4096, sizeof(s_stream) };
  for (size_t p = 0; p < sizeof(pieces) / sizeof(pieces[0]); p++)
  {
    size_t expected;
    size_t const size = _stream(&expected);
    az_mqtt_websocket_options ws = az_mqtt_websocket_options_default();
    int pings;
    assert_int_equal(_deframe_all(&ws, size, pieces[p], &pings), expected);
    assert_memory_equal(s_stream, s_expected, expected);
    assert_int_equal(pings, 2);
  }
}

/** @brief Deframe @p frames (one buffer) and return the last event. */
static _az_mqtt_websocket_event _last_event(uint8_t const* frames, size_t size)
{
  az_mqtt_websocket_options ws = az_mqtt_websocket_options_default();
  static uint8_t out[256];
  _az_mqtt_websocket_event event = _AZ_MQTT_WEBSOCKET_EVENT_DATA;
  int32_t in = 0;
  while (event != _AZ_MQTT_WEBSOCKET_EVENT_ERROR && in < (int32_t)size)
  {
    int32_t consumed;
    int32_t produced;
    event = _az_mqtt_websocket_deframe(
        &ws, frames + in, (int32_t)size - in, out, &consumed, &produced);
    in += consumed;
  }
  return event;
}

static void close_frames_carry_their_code(void** state)
{
  (void)state;
  uint8_t frames[16];
  size_t n = _frame(frames, 0x82, (uint8_t const*)"ab", 2);
  n += _frame(frames + n, 0x88, (uint8_t const*)"\x03\xe9" "bye", 5);
  az_mqtt_websocket_options ws = az_mqtt_websocket_options_default();
  uint8_t out[16];
  int32_t consumed;
  int32_t produced;
  assert_int_equal(
      _az_mqtt_websocket_deframe(&ws, frames, (int32_t)n, out, &consumed, &produced),
      _AZ_MQTT_WEBSOCKET_EVENT_CLOSE);
  assert_int_equal(consumed, (int32_t)n);
  assert_int_equal(produced, 2);
  assert_int_equal(_az_mqtt_websocket_close_code(&ws), 1001);

  n = _frame(frames, 0x88, (uint8_t const*)"", 0);
  ws = az_mqtt_websocket_options_default();
  assert_int_equal(
      _az_mqtt_websocket_deframe(&ws, frames, (int32_t)n, out, &consumed, &produced),
      _AZ_MQTT_WEBSOCKET_EVENT_CLOSE);
  assert_int_equal(_az_mqtt_websocket_close_code(&ws), 1005);
}

static void frames_rfc_6455_forbids_are_refused(void** state)
{
  (void)state;
  static uint8_t const cases[][12] = {
    { 0x82, 0x81, 1, 2, 3, 4, 'x' }, // Masked by the server.
    { 0xC2, 0x01, 'x' }, // RSV1
    { 0xA2, 0x01, 'x' }, // RSV2
    { 0x92, 0x01, 'x' }, // RSV3
    { 0x81, 0x01, 'x' }, // Text
    { 0x83, 0x01, 'x' }, // Reserved data opcode
    { 0x8B, 0x00 }, // Reserved control opcode
    { 0x80, 0x01, 'x' }, // Continuation, no message
    { 0x02, 0x01, 'x', 0x82, 0x01, 'y' }, // Binary inside a message
    { 0x09, 0x00 }, // Fragmented ping
    { 0x89, 0x7E, 0x00, 0x7E }, // Ping over 125 bytes
    { 0x88, 0x01, 0x03 }, // Close with a 1-byte body
    { 0x82, 0x7F, 0x80, 0, 0, 0, 0, 0, 0, 0 }, // 64-bit length with the top bit set
  };
  static size_t const sizes[] = { 7, 3, 3, 3, 3, 3, 2, 3, 6, 2, 4, 3, 10 };
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
  {
    assert_int_equal(_last_event(cases[i], sizes[i]), _AZ_MQTT_WEBSOCKET_EVENT_ERROR);
  }
  // The same, allowed.
  uint8_t const ok[] = { 0x02, 0x01, 'x', 0x89, 0x00, 0x80, 0x01, 'y', 0x8A, 0x7D };
  assert_int_equal(_last_event(ok, sizeof(ok) - 2), _AZ_MQTT_WEBSOCKET_EVENT_DATA);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(sha1_matches_known_digests),
    cmocka_unit_test(the_request_follows_rfc_6455),
    cmocka_unit_test(the_longest_request_fits),
    cmocka_unit_test(bad_hosts_and_paths_are_refused),
    cmocka_unit_test(a_valid_reply_upgrades_in_any_number_of_pieces),
    cmocka_unit_test(a_long_reply_is_accepted),
    cmocka_unit_test(invalid_replies_are_refused),
    cmocka_unit_test(frame_headers_use_the_shortest_length),
    cmocka_unit_test(masking_continues_across_pieces),
    cmocka_unit_test(frames_deframe_in_place_in_any_pieces),
    cmocka_unit_test(close_frames_carry_their_code),
    cmocka_unit_test(frames_rfc_6455_forbids_are_refused),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
