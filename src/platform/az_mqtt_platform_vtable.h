// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_platform_vtable.h
 * @brief Internal: included once, at the end of a platform transport, which defines
 * _platform_transport (starting with az_mqtt_transport base) and the _platform_* functions.
 * Defines its vtable, az_mqtt_transport_init() and az_mqtt_transport_sizeof().
 */

#define _PLATFORM(t) ((_platform_transport*)(t))

static az_result _vt_connect_start(
    az_mqtt_transport* t,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  return _platform_connect_start(_PLATFORM(t), host, port, tls_options);
}

static az_result _vt_connect_poll(az_mqtt_transport* t, int32_t timeout_ms)
{
  return _platform_connect_poll(_PLATFORM(t), timeout_ms);
}

static az_result _vt_send(az_mqtt_transport* t, az_span data)
{
  return _platform_send(_PLATFORM(t), data);
}

static az_result
_vt_receive(az_mqtt_transport* t, az_span buffer, int32_t timeout_ms, az_span* out_received)
{
  return _platform_receive(_PLATFORM(t), buffer, timeout_ms, out_received);
}

static void _vt_close(az_mqtt_transport* t) { _platform_close(_PLATFORM(t)); }

static az_result _vt_set_proxy(az_mqtt_transport* t, az_mqtt_proxy_options const* proxy)
{
  return _platform_set_proxy(_PLATFORM(t), proxy);
}

static void
_vt_set_error_callback(az_mqtt_transport* t, az_mqtt_transport_error_fn callback, void* context)
{
  _platform_set_error_callback(_PLATFORM(t), callback, context);
}

static az_mqtt_transport_vtable const _platform_vtable = {
  _vt_connect_start, _vt_connect_poll, _vt_send,     _vt_receive,
  NULL,              _vt_close,        _vt_set_proxy, _vt_set_error_callback,
};

AZ_NODISCARD int32_t az_mqtt_transport_sizeof(void) { return (int32_t)sizeof(_platform_transport); }

AZ_NODISCARD az_result az_mqtt_transport_init(az_mqtt_transport* transport)
{
  _az_PRECONDITION_NOT_NULL(transport);
  az_result const rc = _platform_init(_PLATFORM(transport));
  transport->vtable = &_platform_vtable;
  return rc;
}
