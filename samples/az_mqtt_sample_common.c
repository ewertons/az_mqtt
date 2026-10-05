// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_sample_common.c
 * @brief Sample settings from environment variables.
 */

#include "az_mqtt_sample_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Environment variable @p name, or @p default_value (NULL: empty) if not set or empty. */
static az_span _env(char const* name, char const* default_value)
{
  char const* value = getenv(name);
  if (value == NULL || value[0] == '\0')
  {
    value = default_value;
  }
  return value == NULL ? AZ_SPAN_EMPTY
                       : az_span_create((uint8_t*)(uintptr_t)value, (int32_t)strlen(value));
}

/** @brief Port in environment variable @p name, or @p default_port; false if invalid. */
static bool _env_port(char const* name, uint16_t default_port, uint16_t* out_port)
{
  char const* value = getenv(name);
  if (value == NULL || value[0] == '\0')
  {
    *out_port = default_port;
    return true;
  }
  char* end = NULL;
  unsigned long const port = strtoul(value, &end, 10);
  if (*end != '\0' || port == 0 || port > UINT16_MAX)
  {
    printf("ERROR: %s=\"%s\" is not a port (1 to 65535)\n", name, value);
    return false;
  }
  *out_port = (uint16_t)port;
  return true;
}

bool az_mqtt_sample_settings_read(
    az_mqtt_sample_settings* settings,
    uint16_t default_port,
    char const* default_client_id)
{
  settings->host = _env("AZ_MQTT_SAMPLE_HOST", "localhost");
  settings->client_id = _env("AZ_MQTT_SAMPLE_CLIENT_ID", default_client_id);
  settings->username = _env("AZ_MQTT_SAMPLE_USERNAME", NULL);
  settings->password = _env("AZ_MQTT_SAMPLE_PASSWORD", NULL);
  settings->tls = az_span_is_content_equal(_env("AZ_MQTT_SAMPLE_TLS", "0"), AZ_SPAN_FROM_STR("1"));
  settings->ca_cert_path = _env("AZ_MQTT_SAMPLE_CA_CERT", NULL);
  settings->client_cert_path = _env("AZ_MQTT_SAMPLE_CLIENT_CERT", NULL);
  settings->client_key_path = _env("AZ_MQTT_SAMPLE_CLIENT_KEY", NULL);
  settings->websocket_path = _env("AZ_MQTT_SAMPLE_WEBSOCKET_PATH", NULL);
  settings->proxy_host = _env("AZ_MQTT_SAMPLE_PROXY_HOST", NULL);
  settings->proxy_username = _env("AZ_MQTT_SAMPLE_PROXY_USERNAME", NULL);
  settings->proxy_password = _env("AZ_MQTT_SAMPLE_PROXY_PASSWORD", NULL);
  return _env_port("AZ_MQTT_SAMPLE_PORT", default_port, &settings->port)
      && _env_port("AZ_MQTT_SAMPLE_PROXY_PORT", 3128, &settings->proxy_port);
}
