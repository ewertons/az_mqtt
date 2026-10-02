// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_websocket.h
 * @brief MQTT over WebSockets (RFC 6455, subprotocol "mqtt").
 */

#ifndef AZ_MQTT_WEBSOCKET_H
#define AZ_MQTT_WEBSOCKET_H

#include <azure/core/az_span.h>

#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/** @brief Longest request path. */
#define AZ_MQTT_WEBSOCKET_PATH_MAX 256

#ifndef AZ_MQTT_WEBSOCKET_SEND_CHUNK
/**
 * @brief Stack buffer frames are masked in on their way out, in bytes (at least 15): a packet
 * larger than it takes several transport sends.
 */
#define AZ_MQTT_WEBSOCKET_SEND_CHUNK 512
#endif

/** @brief Path Azure IoT Hub serves MQTT over WebSockets on. */
#define AZ_MQTT_WEBSOCKET_PATH_IOT_HUB "/$iothub/websocket"

/**
 * @brief MQTT over WebSockets, set with a client's websocket_options.
 *
 * The WebSocket runs over the client's TCP or TLS connection, through its proxy if any: set the
 * client's port to the server's WebSocket listener (typically 443 with TLS, 80 without).
 *
 * The client keeps its WebSocket state here: one instance serves one client, and must stay valid
 * for the client's lifetime.
 */
typedef struct
{
  /**
   * @brief Request path: '/' then printable ASCII, no space (empty: "/mqtt"). Used, not copied.
   */
  az_span path;

  struct
  {
    uint64_t frame_left;
    uint32_t http[2];
    uint8_t accept[28];
    uint8_t line[80];
    uint8_t header[10];
    uint8_t control[125];
    uint8_t line_size;
    uint8_t header_size;
    uint8_t control_size;
    uint8_t opcode;
    uint8_t stage;
    uint8_t flags;
  } _internal;
} az_mqtt_websocket_options;

/** @brief Defaults: path "/mqtt". */
AZ_NODISCARD AZ_INLINE az_mqtt_websocket_options az_mqtt_websocket_options_default(void)
{
  az_mqtt_websocket_options options = { 0 };
  options.path = AZ_SPAN_EMPTY;
  return options;
}

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT_WEBSOCKET_H
