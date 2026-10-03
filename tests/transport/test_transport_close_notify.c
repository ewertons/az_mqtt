// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_transport_close_notify.c
 * @brief TLS layer: close_notify on close, and the native errors met sending it, over a layer
 * that fails on demand between TLS and the socket (any TLS backend).
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "az_mqtt_io_layers_internal.h"
#include "test_native_errors.h"
#include "test_server.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(AZ_MQTT_TEST_BACKEND_NONE)

/** @brief Native error the faulty layer reports when a send fails. */
#define _SEND_ERRNO 32 // EPIPE

/** @brief Passes everything to the socket layer below, or fails sends once told to. */
typedef struct
{
  _az_mqtt_io_layer layer; ///< Must be first.
  _az_mqtt_io_layer* lower;
  az_mqtt_transport_error_fn callback;
  void* context;
  bool fail_sends;
  int sends_failed;
} _faulty;

#define _F(t) ((_faulty*)(t))
#define _LOWER(t) (&_F(t)->lower->base)

static az_result _fail_send(_faulty* f)
{
  f->sends_failed++;
  if (f->callback != NULL)
  {
    az_mqtt_native_error const e
        = { AZ_MQTT_NATIVE_ERROR_SOCKET, _SEND_ERRNO, AZ_MQTT_ERROR_CONNECTION_CLOSED, 0 };
    f->callback(&e, f->context);
  }
  return AZ_MQTT_ERROR_CONNECTION_CLOSED;
}

static az_result _connect_start(
    az_mqtt_transport* t,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls)
{
  return az_mqtt_transport_connect_start(_LOWER(t), host, port, tls);
}

static az_result _connect_poll(az_mqtt_transport* t, int32_t timeout_ms)
{
  return az_mqtt_transport_connect_poll(_LOWER(t), timeout_ms);
}

static az_result _send(az_mqtt_transport* t, az_span data)
{
  return _F(t)->fail_sends ? _fail_send(_F(t)) : az_mqtt_transport_send(_LOWER(t), data);
}

static az_result
_receive(az_mqtt_transport* t, az_span buffer, int32_t timeout_ms, az_span* out_received)
{
  return az_mqtt_transport_receive(_LOWER(t), buffer, timeout_ms, out_received);
}

static void _close(az_mqtt_transport* t) { az_mqtt_transport_close(_LOWER(t)); }

static void
_set_error_callback(az_mqtt_transport* t, az_mqtt_transport_error_fn callback, void* context)
{
  _F(t)->callback = callback;
  _F(t)->context = context;
  az_mqtt_transport_set_error_callback(_LOWER(t), callback, context);
}

static az_result _send_some(az_mqtt_transport* t, az_span data, int32_t timeout_ms, int32_t* out)
{
  *out = 0;
  return _F(t)->fail_sends ? _fail_send(_F(t))
                           : _az_mqtt_io_layer_send_some(_F(t)->lower, data, timeout_ms, out);
}

static az_mqtt_transport_vtable const _vtable = {
  _connect_start, _connect_poll, _send, _receive, NULL, _close, NULL, _set_error_callback,
};

static _az_mqtt_io_layer_ops const _ops = { _send_some };

typedef struct
{
  test_server* server;
  az_mqtt_transport* tls;
  _az_mqtt_io_layer* socket;
  _faulty faulty;
  test_native_errors native;
} fixture;

static void _setup(fixture* f)
{
  memset(f, 0, sizeof(*f));
  test_server_options o = test_server_options_default(); // TLS
  f->server = test_server_start(&o);
  assert_non_null(f->server);
  f->socket = (_az_mqtt_io_layer*)calloc(1, (size_t)_az_mqtt_socket_transport_sizeof());
  f->tls = (az_mqtt_transport*)calloc(1, (size_t)_az_mqtt_tls_transport_sizeof());
  assert_non_null(f->socket);
  assert_non_null(f->tls);
  assert_int_equal(_az_mqtt_socket_transport_init(f->socket), AZ_OK);
  f->faulty.layer.base.vtable = &_vtable;
  f->faulty.layer.ops = &_ops;
  f->faulty.lower = f->socket;
  assert_int_equal(_az_mqtt_tls_transport_init(f->tls, &f->faulty.layer), AZ_OK);
  az_mqtt_transport_set_error_callback(f->tls, test_native_errors_record, &f->native);
}

static void _teardown(fixture* f)
{
  az_mqtt_transport_close(f->tls);
  free(f->tls);
  free(f->socket);
  test_server_stop(f->server);
}

/** @brief Connect over TLS, then exchange CONNECT/CONNACK: the session is up and usable. */
static void _connect(fixture* f)
{
  az_mqtt_tls_options tls = az_mqtt_tls_options_default();
  tls.ca_cert_path = az_span_create_from_str((char*)(uintptr_t)test_server_ca_path(f->server));
  assert_int_equal(
      az_mqtt_transport_connect(
          f->tls, AZ_SPAN_FROM_STR("localhost"), test_server_port(f->server), &tls),
      AZ_OK);
  static uint8_t connect_v5[] = { 0x10, 0x0E, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x05,
                                  0x02, 0x00, 0x3C, 0x00, 0x00, 0x01, 'c' };
  assert_int_equal(az_mqtt_transport_send(f->tls, AZ_SPAN_FROM_BUFFER(connect_v5)), AZ_OK);
  uint8_t buf[8];
  az_span got = AZ_SPAN_EMPTY;
  assert_int_equal(az_mqtt_transport_receive(f->tls, AZ_SPAN_FROM_BUFFER(buf), 3000, &got), AZ_OK);
  assert_true(az_span_size(got) > 0 && buf[0] == 0x20);
  test_native_errors_clear(&f->native);
}

/** @brief Wait up to 2 s for the server to count @p n close_notify. */
static int _close_notifies(fixture* f, int n)
{
  for (int i = 0; i < 200 && test_server_close_notifies(f->server) < n; i++)
  {
    struct timespec const tick = { 0, 10 * 1000000L };
    nanosleep(&tick, NULL);
  }
  return test_server_close_notifies(f->server);
}

static void close_sends_close_notify_without_errors(void** state)
{
  (void)state;
  fixture f;
  _setup(&f);
  _connect(&f);
  az_mqtt_transport_close(f.tls);
  assert_int_equal(_close_notifies(&f, 1), 1);
  assert_int_equal(f.native.count, 0);
  _teardown(&f);
}

/** @brief The errors met sending close_notify reach the callback, from @p end. */
static void _close_notify_send_fails(void (*end)(az_mqtt_transport*))
{
  fixture f;
  _setup(&f);
  _connect(&f);
  f.faulty.fail_sends = true;
  end(f.tls);
  assert_true(f.faulty.sends_failed >= 1); // close_notify was attempted.
  assert_true(f.native.count >= 1);
  // The failure below first, with its own result, stamped with the TLS layer's attempt.
  az_mqtt_native_error const* const e = &f.native.errors[0];
  assert_int_equal(e->source, AZ_MQTT_NATIVE_ERROR_SOCKET);
  assert_int_equal(e->code, _SEND_ERRNO);
  assert_int_equal(e->result, AZ_MQTT_ERROR_CONNECTION_CLOSED);
  assert_int_equal(e->connect_attempt, 1);
#if defined(AZ_MQTT_TEST_BACKEND_MBEDTLS)
  // Then the TLS library's, with the failure below as its result.
  assert_int_equal(f.native.count, 2);
  assert_int_equal(f.native.errors[1].source, AZ_MQTT_NATIVE_ERROR_TLS);
  assert_int_equal(f.native.errors[1].code, -0x004E); // MBEDTLS_ERR_NET_SEND_FAILED
  assert_int_equal(f.native.errors[1].result, AZ_MQTT_ERROR_CONNECTION_CLOSED);
  assert_int_equal(f.native.errors[1].connect_attempt, 1);
#else
  assert_int_equal(f.native.count, 1); // OpenSSL queues no error for a failed BIO write.
#endif
  // Tried once: closing again sends nothing more.
  int const failed = f.faulty.sends_failed;
  int const reported = f.native.count;
  az_mqtt_transport_close(f.tls);
  assert_int_equal(f.faulty.sends_failed, failed);
  assert_int_equal(f.native.count, reported);
  _teardown(&f);
}

static void close_reports_errors_sending_close_notify(void** state)
{
  (void)state;
  _close_notify_send_fails(az_mqtt_transport_close);
}

static void shutdown_reports_errors_sending_close_notify(void** state)
{
  (void)state;
  _close_notify_send_fails(az_mqtt_transport_shutdown);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(close_sends_close_notify_without_errors),
    cmocka_unit_test(close_reports_errors_sending_close_notify),
    cmocka_unit_test(shutdown_reports_errors_sending_close_notify),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

#else
int main(void) { return 0; } // No TLS backend: no close_notify.
#endif
