// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_websocket.c
 * @brief Internal: WebSocket client handshake and framing (RFC 6455). See
 * az_mqtt_websocket_internal.h.
 */

#include "az_mqtt_websocket_internal.h"

#include "../platform/az_mqtt_http_connect.h"

#include <azure/core/az_base64.h>

#include <string.h>

/** @brief Shorthand for the WebSocket state. */
#define _W(ws) ((ws)->_internal)

/** @brief options->_internal.flags. */
enum
{
  _FLAG_UPGRADE = 0x01, ///< "Upgrade: websocket" seen.
  _FLAG_CONNECTION = 0x02, ///< "Connection: ... upgrade ..." seen.
  _FLAG_ACCEPT = 0x04, ///< The expected Sec-WebSocket-Accept seen.
  _FLAG_INVALID = 0x08, ///< A header that fails the handshake seen.
  _FLAG_LINE_TRUNCATED = 0x10, ///< The header line did not fit _internal.line.
  _FLAG_IN_MESSAGE = 0x20, ///< A fragmented binary message awaits its continuation frames.
};

// The HTTP parser state lives in the caller's options, as plain words.
typedef char _http_state_fits
    [sizeof(_az_mqtt_http_reply) <= sizeof(((az_mqtt_websocket_options*)0)->_internal.http) ? 1
                                                                                           : -1];

// ──────────────────────── SHA-1 ──────────────────────────────

static uint32_t _rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void _sha1_block(uint32_t h[5], uint8_t const* block)
{
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
  {
    w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16
        | (uint32_t)block[4 * i + 2] << 8 | (uint32_t)block[4 * i + 3];
  }
  for (int i = 16; i < 80; i++)
  {
    w[i] = _rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++)
  {
    uint32_t f;
    uint32_t k;
    if (i < 20)
    {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    }
    else if (i < 40)
    {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    }
    else if (i < 60)
    {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    }
    else
    {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    uint32_t const t = _rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = _rotl(b, 30);
    b = a;
    a = t;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

void _az_mqtt_sha1(uint8_t const* data, size_t size, uint8_t out[20])
{
  uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
  size_t done = 0;
  for (; size - done >= 64; done += 64)
  {
    _sha1_block(h, data + done);
  }
  // Last block(s): the rest, 0x80, zeros, then the length in bits (big-endian).
  uint8_t tail[128] = { 0 };
  size_t const rest = size - done;
  if (rest > 0)
  {
    memcpy(tail, data + done, rest);
  }
  tail[rest] = 0x80;
  size_t const tail_size = rest < 56 ? 64 : 128;
  uint64_t const bits = (uint64_t)size * 8;
  for (int i = 0; i < 8; i++)
  {
    tail[tail_size - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));
  }
  _sha1_block(h, tail);
  if (tail_size == 128)
  {
    _sha1_block(h, tail + 64);
  }
  for (int i = 0; i < 20; i++)
  {
    out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
  }
}

// ──────────────────────── Handshake ──────────────────────────

/** @brief Whether every byte of @p path is printable ASCII other than space. */
static bool _is_valid_path(az_span path)
{
  uint8_t const* p = az_span_ptr(path);
  int32_t const size = az_span_size(path);
  if (size > AZ_MQTT_WEBSOCKET_PATH_MAX || (size > 0 && p[0] != '/'))
  {
    return false;
  }
  for (int32_t i = 0; i < size; i++)
  {
    if (p[i] <= ' ' || p[i] >= 0x7F)
    {
      return false;
    }
  }
  return true;
}

az_result _az_mqtt_websocket_check(az_mqtt_websocket_options const* options)
{
  return options == NULL || _is_valid_path(options->path) ? AZ_OK : AZ_MQTT_ERROR_INVALID_CONFIG;
}

/** @brief Whether @p host can go in a Host header: non-empty, bounded, no break or space. */
static bool _is_valid_host(az_span host)
{
  uint8_t const* p = az_span_ptr(host);
  int32_t const size = az_span_size(host);
  if (size == 0 || size > 255)
  {
    return false;
  }
  for (int32_t i = 0; i < size; i++)
  {
    if (p[i] == '\r' || p[i] == '\n' || p[i] == '\0' || p[i] == ' ')
    {
      return false;
    }
  }
  return true;
}

/** @brief Copy @p text to @p *rest; false if it does not fit. */
static bool _append(az_span* rest, az_span text)
{
  if (az_span_size(*rest) < az_span_size(text))
  {
    return false;
  }
  *rest = az_span_copy(*rest, text);
  return true;
}

/** @brief The Sec-WebSocket-Accept value for @p key (base64 of 16 bytes): 28 bytes. */
static az_result _accept_for(az_span key, uint8_t out[28])
{
  static char const guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t joined[24 + sizeof(guid) - 1];
  memcpy(joined, az_span_ptr(key), 24);
  memcpy(joined + 24, guid, sizeof(guid) - 1);
  uint8_t digest[20];
  _az_mqtt_sha1(joined, sizeof(joined), digest);
  int32_t written = 0;
  return az_base64_encode(
      az_span_create(out, 28), az_span_create(digest, (int32_t)sizeof(digest)), &written);
}

az_result _az_mqtt_websocket_request(
    az_mqtt_websocket_options* ws,
    az_span host,
    uint16_t port,
    bool tls,
    uint8_t const nonce[16],
    az_span buffer,
    int32_t* out_size)
{
  if (!_is_valid_host(host) || !_is_valid_path(ws->path))
  {
    return AZ_MQTT_ERROR_INVALID_CONFIG;
  }
  uint8_t key[24];
  int32_t written = 0;
  memset(&_W(ws), 0, sizeof(_W(ws)));
  az_result rc = az_base64_encode(
      AZ_SPAN_FROM_BUFFER(key), az_span_create((uint8_t*)(uintptr_t)nonce, 16), &written);
  rc = az_result_succeeded(rc) ? _accept_for(AZ_SPAN_FROM_BUFFER(key), _W(ws).accept) : rc;
  if (az_result_failed(rc))
  {
    return rc;
  }
  _az_mqtt_http_reply reply;
  _az_mqtt_http_reply_init(&reply);
  memcpy(_W(ws).http, &reply, sizeof(reply));
  _W(ws).stage = _AZ_MQTT_WEBSOCKET_UPGRADING;

  bool const ipv6 = az_span_find(host, AZ_SPAN_FROM_STR(":")) >= 0;
  bool const default_port = port == (tls ? 443 : 80);
  az_span rest = buffer;
  bool ok = _append(&rest, AZ_SPAN_FROM_STR("GET "))
      && _append(&rest, az_span_size(ws->path) > 0 ? ws->path : AZ_SPAN_FROM_STR("/mqtt"))
      && _append(&rest, AZ_SPAN_FROM_STR(" HTTP/1.1\r\nHost: "))
      && (!ipv6 || _append(&rest, AZ_SPAN_FROM_STR("["))) && _append(&rest, host)
      && (!ipv6 || _append(&rest, AZ_SPAN_FROM_STR("]")));
  if (ok && !default_port)
  {
    ok = _append(&rest, AZ_SPAN_FROM_STR(":"))
        && az_result_succeeded(az_span_u32toa(rest, port, &rest));
  }
  ok = ok
      && _append(
           &rest,
           AZ_SPAN_FROM_STR("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Key: "))
      && _append(&rest, AZ_SPAN_FROM_BUFFER(key))
      && _append(
           &rest,
           AZ_SPAN_FROM_STR("\r\nSec-WebSocket-Version: 13\r\n"
                            "Sec-WebSocket-Protocol: mqtt\r\n\r\n"));
  if (!ok)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  *out_size = az_span_size(buffer) - az_span_size(rest);
  return AZ_OK;
}

static uint8_t _lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + ('a' - 'A')) : c; }

/** @brief Whether @p a equals @p b, ignoring ASCII case. */
static bool _equals_ignore_case(az_span a, az_span b)
{
  if (az_span_size(a) != az_span_size(b))
  {
    return false;
  }
  for (int32_t i = 0; i < az_span_size(a); i++)
  {
    if (_lower(az_span_ptr(a)[i]) != _lower(az_span_ptr(b)[i]))
    {
      return false;
    }
  }
  return true;
}

/** @brief @p text without leading and trailing spaces and tabs. */
static az_span _trim(az_span text)
{
  uint8_t const* p = az_span_ptr(text);
  int32_t start = 0;
  int32_t end = az_span_size(text);
  while (start < end && (p[start] == ' ' || p[start] == '\t'))
  {
    start++;
  }
  while (end > start && (p[end - 1] == ' ' || p[end - 1] == '\t'))
  {
    end--;
  }
  return az_span_slice(text, start, end);
}

/** @brief Whether the comma-separated @p list has @p token, ignoring case. */
static bool _has_token(az_span list, az_span token)
{
  while (az_span_size(list) > 0)
  {
    int32_t comma = az_span_find(list, AZ_SPAN_FROM_STR(","));
    int32_t const end = comma < 0 ? az_span_size(list) : comma;
    if (_equals_ignore_case(_trim(az_span_slice(list, 0, end)), token))
    {
      return true;
    }
    list = comma < 0 ? AZ_SPAN_EMPTY : az_span_slice_to_end(list, comma + 1);
  }
  return false;
}

/** @brief Check the header line in _internal.line against the handshake. */
static void _on_header(az_mqtt_websocket_options* ws)
{
  az_span const line = az_span_create(_W(ws).line, _W(ws).line_size);
  int32_t const colon = az_span_find(line, AZ_SPAN_FROM_STR(":"));
  if (colon < 0)
  {
    return; // Not a field; nothing the handshake depends on.
  }
  az_span const name = az_span_slice(line, 0, colon);
  az_span const value = _trim(az_span_slice_to_end(line, colon + 1));
  bool const truncated = (_W(ws).flags & _FLAG_LINE_TRUNCATED) != 0;
  uint8_t flag = 0;
  bool valid = true;
  if (_equals_ignore_case(name, AZ_SPAN_FROM_STR("Upgrade")))
  {
    flag = _FLAG_UPGRADE;
    valid = _equals_ignore_case(value, AZ_SPAN_FROM_STR("websocket"));
  }
  else if (_equals_ignore_case(name, AZ_SPAN_FROM_STR("Connection")))
  {
    flag = _FLAG_CONNECTION;
    valid = _has_token(value, AZ_SPAN_FROM_STR("upgrade"));
  }
  else if (_equals_ignore_case(name, AZ_SPAN_FROM_STR("Sec-WebSocket-Accept")))
  {
    flag = _FLAG_ACCEPT;
    valid = az_span_is_content_equal(value, AZ_SPAN_FROM_BUFFER(_W(ws).accept));
  }
  else if (_equals_ignore_case(name, AZ_SPAN_FROM_STR("Sec-WebSocket-Protocol")))
  {
    valid = az_span_is_content_equal(value, AZ_SPAN_FROM_STR("mqtt"));
  }
  else if (_equals_ignore_case(name, AZ_SPAN_FROM_STR("Sec-WebSocket-Extensions")))
  {
    valid = false; // None were offered.
  }
  else
  {
    return;
  }
  _W(ws).flags |= valid && !truncated ? flag : _FLAG_INVALID;
}

az_result _az_mqtt_websocket_reply_parse(
    az_mqtt_websocket_options* ws,
    az_span data,
    int32_t* out_consumed,
    uint16_t* out_status)
{
  _az_mqtt_http_reply reply;
  memcpy(&reply, _W(ws).http, sizeof(reply));
  az_result rc = AZ_MQTT_ERROR_TIMEOUT;
  int32_t i = 0;
  while (rc == AZ_MQTT_ERROR_TIMEOUT && i < az_span_size(data))
  {
    uint8_t const c = az_span_ptr(data)[i++];
    switch (_az_mqtt_http_reply_step(&reply, c))
    {
      case _AZ_MQTT_HTTP_HEADER_BYTE:
        if (_W(ws).line_size < sizeof(_W(ws).line))
        {
          _W(ws).line[_W(ws).line_size++] = c;
        }
        else
        {
          _W(ws).flags |= _FLAG_LINE_TRUNCATED;
        }
        break;
      case _AZ_MQTT_HTTP_HEADER_END:
        _on_header(ws);
        _W(ws).line_size = 0;
        _W(ws).flags &= (uint8_t)~_FLAG_LINE_TRUNCATED;
        break;
      case _AZ_MQTT_HTTP_END:
      {
        uint8_t const required = _FLAG_UPGRADE | _FLAG_CONNECTION | _FLAG_ACCEPT;
        bool const upgraded = reply.status == 101
            && (_W(ws).flags & (required | _FLAG_INVALID)) == required;
        rc = upgraded ? AZ_OK : AZ_MQTT_ERROR_WEBSOCKET;
        break;
      }
      case _AZ_MQTT_HTTP_MALFORMED:
        rc = AZ_MQTT_ERROR_WEBSOCKET;
        break;
      default:
        break;
    }
  }
  memcpy(_W(ws).http, &reply, sizeof(reply));
  *out_consumed = i;
  *out_status = reply.status;
  if (rc != AZ_MQTT_ERROR_TIMEOUT)
  {
    _W(ws).flags = 0;
    _W(ws).stage = rc == AZ_OK ? _AZ_MQTT_WEBSOCKET_OPEN : _AZ_MQTT_WEBSOCKET_CLOSED;
  }
  return rc;
}

// ──────────────────────── Framing ────────────────────────────

int32_t _az_mqtt_websocket_frame_header(
    uint8_t* out,
    uint8_t opcode,
    uint64_t size,
    uint8_t const mask[4])
{
  int32_t n = 2;
  out[0] = (uint8_t)(0x80 | opcode); // FIN: the client never fragments.
  if (size < 126)
  {
    out[1] = (uint8_t)(0x80 | size);
  }
  else if (size <= UINT16_MAX)
  {
    out[1] = 0x80 | 126;
    out[n++] = (uint8_t)(size >> 8);
    out[n++] = (uint8_t)size;
  }
  else
  {
    out[1] = 0x80 | 127;
    for (int i = 7; i >= 0; i--)
    {
      out[n++] = (uint8_t)(size >> (8 * i));
    }
  }
  memcpy(out + n, mask, 4);
  return n + 4;
}

void _az_mqtt_websocket_mask(uint8_t* data, int32_t size, uint8_t const mask[4], uint64_t offset)
{
  for (int32_t i = 0; i < size; i++)
  {
    data[i] ^= mask[(offset + (uint64_t)i) & 3];
  }
}

/**
 * @brief Validate a frame header from its first two bytes, @p b0 and @p b1.
 * @return Its full size (2, 4 or 10), or 0 if the frame is not allowed.
 */
static int32_t _header_size(az_mqtt_websocket_options const* ws, uint8_t b0, uint8_t b1)
{
  uint8_t const opcode = b0 & 0x0F;
  bool const fin = (b0 & 0x80) != 0;
  bool const in_message = (_W(ws).flags & _FLAG_IN_MESSAGE) != 0;
  uint8_t const length = b1 & 0x7F;
  bool allowed;
  switch (opcode)
  {
    case 0x0: // Continuation
      allowed = in_message;
      break;
    case _AZ_MQTT_WEBSOCKET_BINARY:
      allowed = !in_message;
      break;
    case _AZ_MQTT_WEBSOCKET_CLOSE:
    case _AZ_MQTT_WEBSOCKET_PING:
    case _AZ_MQTT_WEBSOCKET_PONG:
      allowed = fin && length <= 125;
      break;
    default: // Text (MQTT is binary), reserved.
      allowed = false;
      break;
  }
  // No extension was negotiated (RSV clear); a server never masks (§5.1).
  if (!allowed || (b0 & 0x70) != 0 || (b1 & 0x80) != 0)
  {
    return 0;
  }
  return length == 127 ? 10 : (length == 126 ? 4 : 2);
}

/** @brief A frame ended: what it means to the caller. */
static _az_mqtt_websocket_event _frame_done(az_mqtt_websocket_options* ws)
{
  switch (_W(ws).opcode & 0x0F)
  {
    case _AZ_MQTT_WEBSOCKET_PING:
      return _AZ_MQTT_WEBSOCKET_EVENT_PING;
    case _AZ_MQTT_WEBSOCKET_CLOSE:
      return _W(ws).control_size == 1 ? _AZ_MQTT_WEBSOCKET_EVENT_ERROR
                                       : _AZ_MQTT_WEBSOCKET_EVENT_CLOSE;
    default: // Pong, data.
      return _AZ_MQTT_WEBSOCKET_EVENT_DATA;
  }
}

_az_mqtt_websocket_event _az_mqtt_websocket_deframe(
    az_mqtt_websocket_options* ws,
    uint8_t const* in,
    int32_t in_size,
    uint8_t* out,
    int32_t* out_consumed,
    int32_t* out_produced)
{
  _az_mqtt_websocket_event event = _AZ_MQTT_WEBSOCKET_EVENT_DATA;
  int32_t i = 0;
  int32_t o = 0;
  while (event == _AZ_MQTT_WEBSOCKET_EVENT_DATA && i < in_size)
  {
    bool const control = (_W(ws).opcode & 0x08) != 0;
    if (_W(ws).frame_left == 0)
    {
      // Header.
      _W(ws).header[_W(ws).header_size++] = in[i++];
      if (_W(ws).header_size < 2)
      {
        continue;
      }
      int32_t const size = _header_size(ws, _W(ws).header[0], _W(ws).header[1]);
      if (size == 0)
      {
        event = _AZ_MQTT_WEBSOCKET_EVENT_ERROR;
        break;
      }
      if (_W(ws).header_size < size)
      {
        continue;
      }
      uint64_t length = _W(ws).header[1] & 0x7F;
      if (size > 2)
      {
        length = 0;
        for (int32_t k = 2; k < size; k++)
        {
          length = length << 8 | _W(ws).header[k];
        }
      }
      if (length >> 63 != 0)
      {
        event = _AZ_MQTT_WEBSOCKET_EVENT_ERROR;
        break;
      }
      uint8_t const b0 = _W(ws).header[0];
      _W(ws).header_size = 0;
      _W(ws).opcode = b0;
      _W(ws).frame_left = length;
      if ((b0 & 0x08) != 0)
      {
        _W(ws).control_size = 0;
      }
      else if ((b0 & 0x80) != 0)
      {
        _W(ws).flags &= (uint8_t)~_FLAG_IN_MESSAGE;
      }
      else
      {
        _W(ws).flags |= _FLAG_IN_MESSAGE;
      }
      if (length == 0)
      {
        event = _frame_done(ws);
      }
      continue;
    }
    // Payload.
    int32_t const take = (uint64_t)(in_size - i) < _W(ws).frame_left
        ? in_size - i
        : (int32_t)_W(ws).frame_left;
    if (control)
    {
      memcpy(_W(ws).control + _W(ws).control_size, in + i, (size_t)take);
      _W(ws).control_size = (uint8_t)(_W(ws).control_size + take);
    }
    else
    {
      memmove(out + o, in + i, (size_t)take);
      o += take;
    }
    i += take;
    _W(ws).frame_left -= (uint64_t)take;
    if (_W(ws).frame_left == 0)
    {
      event = _frame_done(ws);
    }
  }
  *out_consumed = i;
  *out_produced = o;
  return event;
}

az_span _az_mqtt_websocket_control(az_mqtt_websocket_options* ws)
{
  return az_span_create(_W(ws).control, _W(ws).control_size);
}

uint16_t _az_mqtt_websocket_close_code(az_mqtt_websocket_options const* ws)
{
  return _W(ws).control_size >= 2
      ? (uint16_t)(_W(ws).control[0] << 8 | _W(ws).control[1])
      : _AZ_MQTT_WEBSOCKET_CLOSE_NO_STATUS;
}
