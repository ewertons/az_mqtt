// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "test_fake_transport.h"

#include <string.h>

#define _FAKE(t) ((test_fake_transport*)(t))

static az_result _fail(test_fake_transport* f, az_result rc)
{
  if (f->failure_errno != 0 && f->error_callback != NULL)
  {
    az_mqtt_native_error const e = { AZ_MQTT_NATIVE_ERROR_SOCKET, f->failure_errno, rc, 0 };
    f->error_callback(&e, f->error_context);
  }
  return rc;
}

static az_result _connect_start(
    az_mqtt_transport* t,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  _FAKE(t)->host = host;
  _FAKE(t)->port = port;
  _FAKE(t)->connects++;
  _FAKE(t)->tls_options = tls_options;
  return AZ_OK;
}

static az_result _connect_poll(az_mqtt_transport* t, int32_t timeout_ms)
{
  (void)timeout_ms;
  if (_FAKE(t)->connect_polls_pending > 0)
  {
    _FAKE(t)->connect_polls_pending--;
    return AZ_MQTT_ERROR_TIMEOUT;
  }
  _FAKE(t)->connected = true;
  return AZ_OK;
}

static az_result _send(az_mqtt_transport* t, az_span data)
{
  test_fake_transport* const f = _FAKE(t);
  f->send_calls++;
  if (f->fail_send_call != 0 && f->send_calls >= f->fail_send_call)
  {
    return _fail(f, AZ_MQTT_ERROR_CONNECTION_CLOSED);
  }
  int32_t const room = az_span_size(f->sent) - f->sent_size;
  int32_t const keep = az_span_size(data) < room ? az_span_size(data) : room;
  if (keep > 0)
  {
    memcpy(az_span_ptr(f->sent) + f->sent_size, az_span_ptr(data), (size_t)keep);
  }
  f->sent_size += az_span_size(data);
  return AZ_OK;
}

static az_result
_receive(az_mqtt_transport* t, az_span buffer, int32_t timeout_ms, az_span* out_received)
{
  (void)timeout_ms;
  test_fake_transport* const f = _FAKE(t);
  int32_t n = f->input_size - f->input_read;
  if (n == 0)
  {
    *out_received = az_span_slice(buffer, 0, 0);
    return az_result_failed(f->end_of_input) ? _fail(f, f->end_of_input) : f->end_of_input;
  }
  n = n < f->chunk ? n : f->chunk;
  n = n < az_span_size(buffer) ? n : az_span_size(buffer);
  memcpy(az_span_ptr(buffer), az_span_ptr(f->input) + f->input_read, (size_t)n);
  f->input_read += n;
  *out_received = az_span_slice(buffer, 0, n);
  return AZ_OK;
}

static void _shutdown(az_mqtt_transport* t) { _FAKE(t)->shutdowns++; }

static void _close(az_mqtt_transport* t)
{
  _FAKE(t)->closes++;
  _FAKE(t)->connected = false;
}

static az_result _set_proxy(az_mqtt_transport* t, az_mqtt_proxy_options const* proxy)
{
  _FAKE(t)->proxy = proxy;
  return AZ_OK;
}

static void
_set_error_callback(az_mqtt_transport* t, az_mqtt_transport_error_fn callback, void* context)
{
  _FAKE(t)->error_callback = callback;
  _FAKE(t)->error_context = context;
}

static az_mqtt_transport_vtable const _vtable = {
  _connect_start, _connect_poll, _send,       _receive,
  _shutdown,      _close,        _set_proxy, _set_error_callback,
};

void test_fake_transport_init(test_fake_transport* fake, az_span input, az_span sent)
{
  memset(fake, 0, sizeof(*fake));
  fake->base.vtable = &_vtable;
  fake->input = input;
  fake->sent = sent;
  fake->chunk = INT32_MAX;
}

void test_fake_transport_feed(test_fake_transport* fake, void const* data, int32_t size)
{
  int32_t const room = az_span_size(fake->input) - fake->input_size;
  int32_t const n = size < room ? size : room;
  memcpy(az_span_ptr(fake->input) + fake->input_size, data, (size_t)n);
  fake->input_size += n;
}
