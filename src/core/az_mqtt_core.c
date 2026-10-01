// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_core.c
 * @brief Session engine shared by the MQTT 3.1.1 and 5.0 clients: connection,
 * packet framing, keep-alive and session state. Zero dynamic allocation.
 */

#include "az_mqtt_codec_internal.h"
#include "az_mqtt_core_internal.h"

#include <azure/core/internal/az_log_internal.h>
#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <string.h>

/** @brief Most packets handled by one process-loop call. */
#define _AZ_MQTT_MAX_PACKETS_PER_LOOP 32

/** @brief Shorthand for the core's internal fields. */
#define _S(core) ((core)->_internal)

static int64_t _get_clock_ms(void) { return az_mqtt_transport_clock_ms(); }

/** @brief Absolute deadline @p timeout_ms from now; -1 (no limit) for a negative timeout. */
static int64_t _deadline(int32_t timeout_ms)
{
  return timeout_ms < 0 ? -1 : _get_clock_ms() + timeout_ms;
}

/** @brief Milliseconds left until @p deadline_ms, 0 if passed, -1 if unlimited. */
static int32_t _remaining(int64_t deadline_ms)
{
  if (deadline_ms < 0)
  {
    return -1;
  }
  int64_t left = deadline_ms - _get_clock_ms();
  return left <= 0 ? 0 : (left > INT32_MAX ? INT32_MAX : (int32_t)left);
}

/** @brief The in-flight entries; @p out_count of them. */
static az_mqtt_inflight_entry* _get_inflight_entries(az_mqtt_core* core, int32_t* out_count)
{
  *out_count
      = az_span_size(_S(core).inflight_control_buffer) / (int32_t)sizeof(az_mqtt_inflight_entry);
  return (az_mqtt_inflight_entry*)az_span_ptr(_S(core).inflight_control_buffer);
}

// ──────────────────────── Logging ────────────────────────────
// Only host, port, packet types and lengths, results and native error codes:
// never credentials, keys, certificates, topics or payloads.

#ifndef AZ_NO_LOGGING
/** @brief Append @p text to @p *out, truncated to what fits. */
static void _log_append(az_span* out, az_span text)
{
  int32_t const room = az_span_size(*out);
  int32_t const size = az_span_size(text) < room ? az_span_size(text) : room;
  *out = az_span_copy(*out, az_span_slice(text, 0, size));
}

/** @brief Append @p value in decimal to @p *out. */
static void _log_append_i32(az_span* out, int32_t value)
{
  az_span rest;
  if (az_result_succeeded(az_span_i32toa(*out, value, &rest)))
  {
    *out = rest;
  }
}

/** @brief Append @p value as 0x and 8 hex digits to @p *out. */
static void _log_append_hex(az_span* out, uint32_t value)
{
  uint8_t digits[10] = { '0', 'x' };
  for (int i = 9; i >= 2; i--, value >>= 4)
  {
    digits[i] = (uint8_t)"0123456789abcdef"[value & 0xFU];
  }
  _log_append(out, AZ_SPAN_FROM_BUFFER(digits));
}

/** @brief Write what @p buffer holds up to @p rest. */
static void _log_write(az_log_classification classification, az_span buffer, az_span rest)
{
  _az_LOG_WRITE(classification, az_span_slice(buffer, 0, az_span_size(buffer) - az_span_size(rest)));
}

/** @brief "connect <host>:<port>[ tls]". */
static void _log_connect(az_mqtt_core* core)
{
  if (!_az_LOG_SHOULD_WRITE(AZ_LOG_MQTT_CONNECTION))
  {
    return;
  }
  uint8_t buffer[128];
  az_span out = AZ_SPAN_FROM_BUFFER(buffer);
  _log_append(&out, AZ_SPAN_FROM_STR("connect "));
  int32_t const host_size = az_span_size(_S(core).hostname);
  _log_append(&out, az_span_slice(_S(core).hostname, 0, host_size > 96 ? 96 : host_size));
  _log_append(&out, AZ_SPAN_FROM_STR(":"));
  _log_append_i32(&out, _S(core).port);
  if (_S(core).tls_options != NULL)
  {
    _log_append(&out, AZ_SPAN_FROM_STR(" tls"));
  }
  _log_write(AZ_LOG_MQTT_CONNECTION, AZ_SPAN_FROM_BUFFER(buffer), out);
}

/** @brief "closed <result> native <source>:<code>". */
static void _log_close(az_mqtt_core* core, az_result reason)
{
  if (!_az_LOG_SHOULD_WRITE(AZ_LOG_MQTT_CONNECTION))
  {
    return;
  }
  az_mqtt_native_error const native = az_mqtt_transport_get_last_native_error(_S(core).transport);
  uint8_t buffer[48];
  az_span out = AZ_SPAN_FROM_BUFFER(buffer);
  _log_append(&out, AZ_SPAN_FROM_STR("closed "));
  _log_append_hex(&out, (uint32_t)reason);
  _log_append(&out, AZ_SPAN_FROM_STR(" native "));
  _log_append_i32(&out, (int32_t)native.source);
  _log_append(&out, AZ_SPAN_FROM_STR(":"));
  _log_append_i32(&out, native.code);
  _log_write(AZ_LOG_MQTT_CONNECTION, AZ_SPAN_FROM_BUFFER(buffer), out);
}

/** @brief "<sent|received> <TYPE> <length>"; @p first_byte is the fixed header's. */
static void _log_packet(az_span direction, uint8_t first_byte, int32_t length)
{
  if (!_az_LOG_SHOULD_WRITE(AZ_LOG_MQTT_PACKET))
  {
    return;
  }
  static char const names[16][12]
      = { "RESERVED", "CONNECT", "CONNACK",     "PUBLISH",  "PUBACK",  "PUBREC",
          "PUBREL",   "PUBCOMP", "SUBSCRIBE",   "SUBACK",   "UNSUBSCRIBE", "UNSUBACK",
          "PINGREQ",  "PINGRESP", "DISCONNECT", "AUTH" };
  uint8_t buffer[40];
  az_span out = AZ_SPAN_FROM_BUFFER(buffer);
  _log_append(&out, direction);
  _log_append(&out, az_span_create_from_str((char*)(uintptr_t)names[first_byte >> 4]));
  _log_append(&out, AZ_SPAN_FROM_STR(" "));
  _log_append_i32(&out, length);
  _log_write(AZ_LOG_MQTT_PACKET, AZ_SPAN_FROM_BUFFER(buffer), out);
}
#else
#define _log_connect(core)
#define _log_close(core, reason)
#define _log_packet(direction, first_byte, length)
#endif // AZ_NO_LOGGING

/** @brief Free every in-flight entry. */
static void _clear_inflight_entries(az_mqtt_core* core)
{
  if (az_span_size(_S(core).inflight_control_buffer) > 0) // az_span_fill: no NULL pointer.
  {
    az_span_fill(_S(core).inflight_control_buffer, 0);
  }
}

void _az_mqtt_core_close(
    az_mqtt_core* core,
    az_result reason)
{
  bool const was_open = _S(core).state != AZ_MQTT_CLIENT_STATE_DISCONNECTED;
  az_mqtt_transport_close(_S(core).transport);
  if (was_open)
  {
    _log_close(core, reason);
  }
  _S(core).state = AZ_MQTT_CLIENT_STATE_DISCONNECTED;
  _S(core).recv_buf_pos = 0;
  _S(core).ping_outstanding = false;
  // Nothing is resent when a session resumes: what was in flight is abandoned.
  _clear_inflight_entries(core);
  _S(core).server_maximum_packet_size = 0;
  // on_closed may reconnect: callers compare generations before touching
  // anything that belonged to the old session.
  _S(core).session_generation++;
  if (was_open && _S(core).on_closed != NULL)
  {
    _S(core).on_closed(core, reason);
  }
}

az_result _az_mqtt_core_send(
    az_mqtt_core* core,
    az_span remaining)
{
  int32_t written = az_span_size(_S(core).send_buffer) - az_span_size(remaining);
  if (written <= 0)
  {
    return AZ_OK;
  }
  if (_S(core).server_maximum_packet_size > 0
      && (uint32_t)written > _S(core).server_maximum_packet_size)
  {
    return AZ_MQTT_ERROR_PACKET_TOO_LARGE;
  }
  az_result rc = az_mqtt_transport_send(
      _S(core).transport, az_span_slice(_S(core).send_buffer, 0, written));
  if (az_result_succeeded(rc))
  {
    _S(core).last_send_time_ms = _get_clock_ms();
    _log_packet(AZ_SPAN_FROM_STR("sent "), az_span_ptr(_S(core).send_buffer)[0], written);
  }
  return rc;
}

// ──────────────────────── In-flight table ────────────────────

void _az_mqtt_core_inflight_init(az_mqtt_core* core, az_span buffer)
{
  int32_t count = az_span_size(buffer) / (int32_t)sizeof(az_mqtt_inflight_entry);
  count = count > UINT16_MAX ? UINT16_MAX : count;
  _S(core).inflight_control_buffer
      = az_span_slice(buffer, 0, count * (int32_t)sizeof(az_mqtt_inflight_entry));
  _clear_inflight_entries(core);
}

/** @brief Whether an outgoing request holds @p packet_id (inbound QoS 2 uses server identifiers). */
static bool _is_outgoing_packet_id_in_use(az_mqtt_core* core, uint16_t packet_id)
{
  int32_t count;
  az_mqtt_inflight_entry const* entries = _get_inflight_entries(core, &count);
  for (int32_t i = 0; i < count; i++)
  {
    uint8_t const kind = entries[i]._internal.kind;
    if (entries[i]._internal.packet_id == packet_id && kind != _AZ_MQTT_INFLIGHT_FREE
        && kind != _AZ_MQTT_INFLIGHT_INBOUND_QOS2)
    {
      return true;
    }
  }
  return false;
}

az_result _az_mqtt_core_inflight_reserve_entry(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t publish_limit,
    az_mqtt_inflight_entry** out_entry)
{
  int32_t count;
  az_mqtt_inflight_entry* entries = _get_inflight_entries(core, &count);
  az_mqtt_inflight_entry* entry = NULL;
  uint16_t publishes = 0;
  for (int32_t i = 0; i < count; i++)
  {
    uint8_t const k = entries[i]._internal.kind;
    if (k == _AZ_MQTT_INFLIGHT_FREE)
    {
      entry = entry != NULL ? entry : &entries[i];
    }
    else if (k <= _AZ_MQTT_INFLIGHT_PUBREL)
    {
      publishes++;
    }
  }
  if (entry == NULL || publishes >= publish_limit)
  {
    return AZ_MQTT_ERROR_FLOW_CONTROL;
  }
  // A free entry leaves at most 65534 identifiers in use, so this ends.
  do
  {
    if (++_S(core).next_packet_id == 0)
    {
      _S(core).next_packet_id = 1;
    }
  } while (_is_outgoing_packet_id_in_use(core, _S(core).next_packet_id));
  entry->_internal.packet_id = _S(core).next_packet_id;
  entry->_internal.kind = (uint8_t)kind;
  *out_entry = entry;
  return AZ_OK;
}

az_mqtt_inflight_entry* _az_mqtt_core_inflight_find_entry(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t packet_id)
{
  int32_t count;
  az_mqtt_inflight_entry* entries = _get_inflight_entries(core, &count);
  for (int32_t i = 0; i < count; i++)
  {
    if (entries[i]._internal.kind == (uint8_t)kind && entries[i]._internal.packet_id == packet_id)
    {
      return &entries[i];
    }
  }
  return NULL;
}

bool _az_mqtt_core_inflight_release_entry(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t packet_id)
{
  az_mqtt_inflight_entry* entry = _az_mqtt_core_inflight_find_entry(core, kind, packet_id);
  if (entry != NULL)
  {
    entry->_internal.kind = _AZ_MQTT_INFLIGHT_FREE;
  }
  return entry != NULL;
}

void _az_mqtt_core_inflight_track_inbound_qos2(
    az_mqtt_core* core,
    uint16_t packet_id,
    bool* out_is_duplicate)
{
  int32_t count;
  az_mqtt_inflight_entry* entries = _get_inflight_entries(core, &count);
  az_mqtt_inflight_entry* free_entry = NULL;
  for (int32_t i = 0; i < count; i++)
  {
    if (entries[i]._internal.kind == _AZ_MQTT_INFLIGHT_INBOUND_QOS2
        && entries[i]._internal.packet_id == packet_id)
    {
      *out_is_duplicate = true;
      return;
    }
    if (free_entry == NULL && entries[i]._internal.kind == _AZ_MQTT_INFLIGHT_FREE)
    {
      free_entry = &entries[i];
    }
  }
  if (free_entry != NULL)
  {
    free_entry->_internal.packet_id = packet_id;
    free_entry->_internal.kind = _AZ_MQTT_INFLIGHT_INBOUND_QOS2;
  }
  *out_is_duplicate = false;
}

az_result _az_mqtt_core_send_tracked_request(
    az_mqtt_core* core,
    az_mqtt_inflight_entry* entry,
    az_result encode_result,
    az_span remaining)
{
  az_result rc = encode_result;
  int32_t const size = az_span_size(_S(core).send_buffer) - az_span_size(remaining);
  if (az_result_succeeded(rc) && _S(core).server_maximum_packet_size > 0
      && (uint32_t)size > _S(core).server_maximum_packet_size)
  {
    rc = AZ_MQTT_ERROR_PACKET_TOO_LARGE; // Refused before sending: the session stays up.
  }
  if (az_result_failed(rc))
  {
    if (entry != NULL)
    {
      entry->_internal.kind = _AZ_MQTT_INFLIGHT_FREE;
    }
    return rc;
  }
  return _az_mqtt_core_send_request(core, remaining);
}

/** @brief Buffer at least @p needed bytes, waiting no later than @p deadline_ms. */
static az_result _ensure_received(
    az_mqtt_core* core,
    int32_t needed,
    int64_t deadline_ms)
{
  while (_S(core).recv_buf_pos < needed)
  {
    az_span free_space = az_span_slice_to_end(_S(core).receive_buffer, _S(core).recv_buf_pos);
    if (az_span_size(free_space) == 0)
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }

    int32_t const wait_ms = _remaining(deadline_ms);
    az_span received;
    az_result rc = az_mqtt_transport_receive(_S(core).transport, free_space, wait_ms, &received);
    if (az_result_failed(rc))
    {
      return rc;
    }
    if (az_span_size(received) == 0)
    {
      if (wait_ms == 0)
      {
        return AZ_MQTT_ERROR_TIMEOUT;
      }
      continue; // Woke early; wait out the rest of the deadline.
    }
    _S(core).recv_buf_pos += az_span_size(received);
  }
  return AZ_OK;
}

/** @brief Drop @p count bytes from the front of the receive buffer. */
static void
_consume_recv(az_mqtt_core* core, int32_t count)
{
  if (count <= 0)
  {
    return;
  }
  int32_t remaining = _S(core).recv_buf_pos - count;
  if (remaining > 0)
  {
    memmove(
        az_span_ptr(_S(core).receive_buffer),
        az_span_ptr(_S(core).receive_buffer) + count,
        (size_t)remaining);
  }
  _S(core).recv_buf_pos = remaining;
}

/**
 * @brief Read one whole packet into the receive buffer.
 *
 * The caller consumes @p out_packet_size bytes (_consume_recv()) after handling the body.
 */
static az_result _read_packet(
    az_mqtt_core* core,
    int64_t deadline_ms,
    az_mqtt_packet_type* out_type,
    uint8_t* out_flags,
    az_span* out_body,
    int32_t* out_packet_size)
{
  // Fixed header: type byte + 1-4 byte Remaining Length.
  az_result rc = _ensure_received(core, 2, deadline_ms);
  if (az_result_failed(rc))
    return rc;

  int32_t header_size = 1;
  int32_t remaining_length = 0;
  int shift = 0;
  bool vbi_complete = false;

  for (int i = 0; i < 4; i++)
  {
    rc = _ensure_received(core, header_size + 1 + i, deadline_ms);
    if (az_result_failed(rc))
      return rc;

    uint8_t b = az_span_ptr(_S(core).receive_buffer)[1 + i];
    remaining_length |= (int32_t)(b & 0x7F) << shift;
    shift += 7;
    if ((b & 0x80) == 0)
    {
      header_size = 2 + i;
      vbi_complete = true;
      break;
    }
  }

  if (!vbi_complete)
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }

  int32_t total_packet_size = header_size + remaining_length;
  rc = _ensure_received(core, total_packet_size, deadline_ms);
  if (az_result_failed(rc))
    return rc;

  az_span header_span = az_span_slice(_S(core).receive_buffer, 0, total_packet_size);
  int32_t decoded_remaining;
  rc = _az_mqtt_decode_fixed_header(&header_span, out_type, out_flags, &decoded_remaining);
  if (az_result_failed(rc))
    return rc;

  *out_body = az_span_slice(header_span, 0, decoded_remaining);
  *out_packet_size = total_packet_size;
  _S(core).last_receive_time_ms = _get_clock_ms();
  // Any packet proves the link is alive, not only a PINGRESP.
  _S(core).ping_outstanding = false;

  return AZ_OK;
}

az_result _az_mqtt_core_connect_start(az_mqtt_core* core, int32_t timeout_ms)
{
  _S(core).recv_buf_pos = 0;
  _S(core).ping_outstanding = false;
  _S(core).connect_sent = false;
  _S(core).timer_ms = _deadline(timeout_ms);
  _S(core).state = AZ_MQTT_CLIENT_STATE_CONNECTING;
  _log_connect(core);

  az_result rc = az_mqtt_transport_connect_start(
      _S(core).transport, _S(core).hostname, _S(core).port, _S(core).tls_options);
  if (az_result_failed(rc))
  {
    _az_mqtt_core_close(core, rc);
  }
  return rc;
}

az_result _az_mqtt_core_connect_wait(az_mqtt_core* core, _az_mqtt_core_dispatch_fn dispatch)
{
  uint32_t const generation = _S(core).session_generation;
  az_result rc = AZ_OK;
  while (az_result_succeeded(rc) && _S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTING
         && _S(core).session_generation == generation)
  {
    rc = _az_mqtt_core_process_loop(core, -1, dispatch);
  }
  if (az_result_failed(rc))
  {
    return rc;
  }
  return _S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTED ? AZ_OK : AZ_MQTT_ERROR_NOT_CONNECTED;
}

/** @brief Whether the connect deadline has passed. */
static bool _connect_expired(az_mqtt_core const* core) { return _remaining(_S(core).timer_ms) == 0; }

/**
 * @brief CONNECTING: progress the transport connect and send CONNECT once it is up.
 *
 * @param[in,out] deadline_ms Wait deadline; lowered to the connect deadline.
 */
static az_result _service_connect(az_mqtt_core* core, int64_t* deadline_ms)
{
  int64_t const connect_deadline = _S(core).timer_ms;
  if (connect_deadline >= 0 && (*deadline_ms < 0 || connect_deadline < *deadline_ms))
  {
    *deadline_ms = connect_deadline;
  }
  if (_S(core).connect_sent)
  {
    return AZ_OK;
  }

  az_result rc = az_mqtt_transport_connect_poll(_S(core).transport, _remaining(*deadline_ms));
  if (rc == AZ_MQTT_ERROR_TIMEOUT)
  {
    return _connect_expired(core) ? rc : AZ_OK; // Not up yet.
  }
  if (az_result_failed(rc))
  {
    return rc;
  }

  // Send the CONNECT encoded at the start of the send buffer.
  az_span packet = _S(core).send_buffer;
  az_mqtt_packet_type type;
  uint8_t flags;
  int32_t remaining_length;
  rc = _az_mqtt_decode_fixed_header(&packet, &type, &flags, &remaining_length);
  if (az_result_succeeded(rc))
  {
    rc = _az_mqtt_core_send(core, az_span_slice_to_end(packet, remaining_length));
  }
  _S(core).connect_sent = az_result_succeeded(rc);
  return rc;
}

/**
 * @brief Send PINGREQ when due and detect a missing response.
 *
 * @param[out] out_next_ms Milliseconds until keep-alive next needs attention; -1 if never.
 */
static az_result _service_keep_alive(
    az_mqtt_core* core,
    int32_t* out_next_ms)
{
  *out_next_ms = -1;
  if (_S(core).state != AZ_MQTT_CLIENT_STATE_CONNECTED || _S(core).keep_alive_seconds == 0)
  {
    return AZ_OK;
  }

  int64_t const now = _get_clock_ms();
  int64_t const keep_alive_ms = (int64_t)_S(core).keep_alive_seconds * 1000;

  if (_S(core).ping_outstanding)
  {
    int64_t const waited = now - _S(core).timer_ms;
    if (waited >= keep_alive_ms)
    {
      return AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT;
    }
    *out_next_ms = (int32_t)(keep_alive_ms - waited);
    return AZ_OK;
  }

  int64_t const idle = now - _S(core).last_send_time_ms;
  if (idle < keep_alive_ms)
  {
    *out_next_ms = (int32_t)(keep_alive_ms - idle);
    return AZ_OK;
  }

  az_span send_buf = _S(core).send_buffer;
  az_result rc = _az_mqtt_encode_pingreq(&send_buf);
  if (az_result_succeeded(rc))
  {
    rc = _az_mqtt_core_send(core, send_buf);
  }
  if (az_result_succeeded(rc))
  {
    // The response window starts once the PINGREQ is out, not before a slow send.
    _S(core).ping_outstanding = true;
    _S(core).timer_ms = _get_clock_ms();
    *out_next_ms = (int32_t)keep_alive_ms;
  }
  return rc;
}

az_result _az_mqtt_core_process_loop(
    az_mqtt_core* core,
    int32_t timeout_ms,
    _az_mqtt_core_dispatch_fn dispatch)
{
  if (_S(core).state == AZ_MQTT_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_MQTT_ERROR_NOT_CONNECTED;
  }

  int32_t next_keep_alive_ms;
  az_result rc = _service_keep_alive(core, &next_keep_alive_ms);
  if (az_result_failed(rc))
  {
    _az_mqtt_core_close(core, rc);
    return rc;
  }

  // Never sleep past the next keep-alive action.
  int32_t wait_ms = timeout_ms;
  if (next_keep_alive_ms >= 0 && (wait_ms < 0 || next_keep_alive_ms < wait_ms))
  {
    wait_ms = next_keep_alive_ms;
  }
  int64_t deadline = _deadline(wait_ms);

  if (_S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTING)
  {
    rc = _service_connect(core, &deadline);
    if (az_result_failed(rc))
    {
      _az_mqtt_core_close(core, rc);
      return rc;
    }
  }

  // Handle every complete packet already available, up to a bound.
  for (int i = 0; i < _AZ_MQTT_MAX_PACKETS_PER_LOOP
       && (_S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTED
           || (_S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTING && _S(core).connect_sent));
       i++)
  {
    bool const connecting = _S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTING;
    az_mqtt_packet_type type;
    uint8_t flags;
    az_span body;
    int32_t packet_size;

    uint32_t const generation = _S(core).session_generation;
    rc = _read_packet(core, deadline, &type, &flags, &body, &packet_size);
    if (rc == AZ_MQTT_ERROR_TIMEOUT && !(connecting && _connect_expired(core)))
    {
      rc = AZ_OK;
      break;
    }
    if (az_result_succeeded(rc))
    {
      _log_packet(AZ_SPAN_FROM_STR("received "), (uint8_t)(type << 4), packet_size);
      // While connecting, only a CONNACK is valid.
      rc = connecting && type != AZ_MQTT_PACKET_TYPE_CONNACK ? AZ_MQTT_ERROR_PROTOCOL
                                                             : dispatch(core, type, flags, body);
      if (_S(core).session_generation != generation)
      {
        // A callback ended this session, and may have connected a new one whose
        // receive buffer must not be touched: stop here.
        return az_result_failed(rc) ? rc : AZ_OK;
      }
      _consume_recv(core, packet_size);
      if (az_result_succeeded(rc) && _S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTING)
      {
        rc = AZ_MQTT_ERROR_NOT_CONNECTED; // CONNACK refused; the callback got the reason.
      }
    }
    if (az_result_failed(rc))
    {
      _az_mqtt_core_close(core, rc);
      return rc;
    }
    if (connecting)
    {
      break; // Connected: later packets belong to the next call.
    }
    deadline = _get_clock_ms(); // Only what is already there from now on.
  }

  // The wait may have been cut to the keep-alive time: act on it now.
  rc = _service_keep_alive(core, &next_keep_alive_ms);
  if (az_result_failed(rc))
  {
    _az_mqtt_core_close(core, rc);
    return rc;
  }
  return AZ_OK;
}
