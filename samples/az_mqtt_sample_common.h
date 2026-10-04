// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_sample_common.h
 * @brief What the samples share: their settings, read from environment variables, and the size of
 * the caller storage they give the library.
 *
 * | Variable                      | Meaning                                     | Default       |
 * |-------------------------------|---------------------------------------------|---------------|
 * | AZ_MQTT_SAMPLE_HOST           | Broker host name or IP address              | localhost     |
 * | AZ_MQTT_SAMPLE_PORT           | Broker port                                 | per sample    |
 * | AZ_MQTT_SAMPLE_CLIENT_ID      | MQTT client identifier                      | per sample    |
 * | AZ_MQTT_SAMPLE_USERNAME       | MQTT user name                              | none          |
 * | AZ_MQTT_SAMPLE_PASSWORD       | MQTT password                               | none          |
 * | AZ_MQTT_SAMPLE_TLS            | 1: TLS (samples where it is optional)       | 0             |
 * | AZ_MQTT_SAMPLE_CA_CERT        | CA certificate file (PEM) trusted for TLS   | system store  |
 * | AZ_MQTT_SAMPLE_CLIENT_CERT    | Client certificate file (PEM), mutual TLS   | none          |
 * | AZ_MQTT_SAMPLE_CLIENT_KEY     | Client private key file (PEM), mutual TLS   | none          |
 * | AZ_MQTT_SAMPLE_WEBSOCKET_PATH | WebSocket request path                      | /mqtt         |
 * | AZ_MQTT_SAMPLE_PROXY_HOST     | HTTP CONNECT proxy host                     | none (direct) |
 * | AZ_MQTT_SAMPLE_PROXY_PORT     | HTTP CONNECT proxy port                     | 3128          |
 * | AZ_MQTT_SAMPLE_PROXY_USERNAME | Proxy user name (HTTP Basic)                | none          |
 * | AZ_MQTT_SAMPLE_PROXY_PASSWORD | Proxy password (HTTP Basic)                 | none          |
 */

#ifndef AZ_MQTT_SAMPLE_COMMON_H
#define AZ_MQTT_SAMPLE_COMMON_H

#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief Send and receive buffer size of the samples: their largest packet. */
#define AZ_MQTT_SAMPLE_BUFFER_SIZE 4096

/**
 * @brief Caller storage for the platform transport: at least az_mqtt_transport_sizeof() bytes
 * (the Schannel transport on Windows is the largest), aligned for its members.
 */
typedef union
{
  uint8_t bytes[256 * 1024];
  void* align_pointer;
  int64_t align_int64;
  double align_double;
} az_mqtt_sample_transport_storage;

/** @brief Sample settings (see the table above). Empty spans: not set. */
typedef struct
{
  az_span host;
  uint16_t port;
  az_span client_id;
  az_span username;
  az_span password;
  bool tls;
  az_span ca_cert_path;
  az_span client_cert_path;
  az_span client_key_path;
  az_span websocket_path;
  az_span proxy_host;
  uint16_t proxy_port;
  az_span proxy_username;
  az_span proxy_password;
} az_mqtt_sample_settings;

/**
 * @brief Read the settings from the environment.
 *
 * @param default_port Port when AZ_MQTT_SAMPLE_PORT is not set.
 * @param default_client_id Client identifier when AZ_MQTT_SAMPLE_CLIENT_ID is not set.
 * @return false (after printing why) if a port is not a number from 1 to 65535.
 */
bool az_mqtt_sample_settings_read(
    az_mqtt_sample_settings* settings,
    uint16_t default_port,
    char const* default_client_id);

#endif // AZ_MQTT_SAMPLE_COMMON_H
