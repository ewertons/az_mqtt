// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_http_connect.h
 * @brief Internal: HTTP CONNECT request and reply (RFC 9110 §9.3.6), without I/O.
 */
#ifndef AZ_MQTT_HTTP_CONNECT_H
#define AZ_MQTT_HTTP_CONNECT_H

#include <az_mqtt/az_mqtt_transport.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdint.h>

/** @brief Room for the longest request _az_mqtt_http_connect_request() builds. */
#define _AZ_MQTT_HTTP_CONNECT_REQUEST_MAX 1024

/** @brief Longest reply accepted (status line and headers). */
#define _AZ_MQTT_HTTP_CONNECT_REPLY_MAX 8192

/** @brief Reply parsing state. */
typedef struct
{
  uint32_t received;
  uint16_t status;
  uint8_t state;
  uint8_t position;
} _az_mqtt_http_connect_reply;

#ifndef AZ_MQTT_NO_PROXY

/** @brief Validate @p proxy (see az_mqtt_transport_set_proxy()); NULL or an empty host is valid. */
AZ_NODISCARD az_result _az_mqtt_http_connect_check(az_mqtt_proxy_options const* proxy);

/**
 * @brief Write the CONNECT request for @p host:@p port, with @p proxy's credentials, to @p buffer.
 *
 * The request holds the credentials: clear @p buffer after use.
 *
 * @retval AZ_MQTT_ERROR_INVALID_CONFIG @p host is empty, too long, or has CR, LF, NUL or a space.
 */
AZ_NODISCARD az_result _az_mqtt_http_connect_request(
    az_mqtt_proxy_options const* proxy,
    az_span host,
    uint16_t port,
    az_span buffer,
    int32_t* out_size);


void _az_mqtt_http_connect_reply_init(_az_mqtt_http_connect_reply* reply);

/**
 * @brief Parse the next bytes of the reply.
 *
 * Takes the reply in any number of pieces, never consumes past its end, skips 1xx interim
 * replies, and accepts LF as well as CRLF line ends.
 *
 * @param[out] out_consumed How many bytes of @p data belong to the reply.
 * @retval AZ_OK The reply is complete, with a 2xx status: the tunnel is open.
 * @retval AZ_MQTT_ERROR_TIMEOUT Incomplete: parse more.
 * @retval AZ_MQTT_ERROR_PROXY_AUTH Status 407.
 * @retval AZ_MQTT_ERROR_PROXY Another status (reply->status), or not HTTP or too long (status 0).
 */
AZ_NODISCARD az_result _az_mqtt_http_connect_reply_parse(
    _az_mqtt_http_connect_reply* reply,
    az_span data,
    int32_t* out_consumed);

#else // Built without proxy support (AZ_MQTT_ENABLE_PROXY=OFF): any proxy is refused.

AZ_NODISCARD AZ_INLINE az_result _az_mqtt_http_connect_check(az_mqtt_proxy_options const* proxy)
{
  return proxy == NULL || az_span_size(proxy->host) == 0 ? AZ_OK : AZ_MQTT_ERROR_NOT_SUPPORTED;
}

#endif // AZ_MQTT_NO_PROXY

#endif // AZ_MQTT_HTTP_CONNECT_H
