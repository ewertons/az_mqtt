// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file
 * @brief az_mqtt on ESP-IDF: connect (TLS or TCP), publish one QoS 1 message, wait for its PUBACK,
 * disconnect. One task owns the client and drives it with process_loop.
 */

#include "sdkconfig.h"

#if defined(CONFIG_AZ_MQTT_SAMPLE_MQTTV5)
#include <az_mqtt5/az_mqtt5_client.h>
#define _V(name) az_mqtt5_##name
#define _CONNACK_CODE(c) ((int)(c)->reason_code)
#define _DISCONNECT(c) az_mqtt5_client_disconnect((c), AZ_MQTT5_REASON_NORMAL_DISCONNECTION)
#else
#include <az_mqtt3/az_mqtt3_client.h>
#define _V(name) az_mqtt3_##name
#define _CONNACK_CODE(c) ((int)(c)->return_code)
#define _DISCONNECT(c) az_mqtt3_client_disconnect((c))
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#define SAMPLE_BUFFER_SIZE 1024
#define SAMPLE_TASK_STACK 8192

static char const* const TAG = "az_mqtt_sample";

#if defined(AZ_MQTT_SAMPLE_HAS_CA_PEM)
extern char const az_mqtt_sample_ca_pem_start[] asm("_binary_az_mqtt_sample_ca_pem_start");
extern char const az_mqtt_sample_ca_pem_end[] asm("_binary_az_mqtt_sample_ca_pem_end");
#endif

static uint8_t s_send_buffer[SAMPLE_BUFFER_SIZE];
static uint8_t s_receive_buffer[SAMPLE_BUFFER_SIZE];
static az_mqtt_inflight_entry s_inflight[4];
static union
{
  uint8_t bytes[8 * 1024];
  void* align_pointer;
  int64_t align_int64;
} s_transport;
static _V(client) s_client;

static bool s_puback_received;
static bool s_acknowledged;

static void on_connack(_V(client) * client, _V(connack_data) const* connack)
{
  (void)client;
  ESP_LOGI(TAG, "CONNACK code=%d", _CONNACK_CODE(connack));
}

static void on_puback(_V(client) * client, _V(ack_data) const* ack)
{
  (void)client;
  ESP_LOGI(TAG, "PUBACK packet_id=%u status=0x%08X", ack->packet_id, (unsigned)ack->status);
  s_puback_received = true;
  s_acknowledged = az_result_succeeded(ack->status);
}

static void on_transport_error(_V(client) * client, az_mqtt_native_error const* error)
{
  (void)client;
  ESP_LOGW(
      TAG,
      "transport error source=%d code=%ld result=0x%08X",
      (int)error->source,
      (long)error->code,
      (unsigned)error->result);
}

#if defined(CONFIG_AZ_MQTT_SAMPLE_USE_TLS) && !defined(AZ_MQTT_SAMPLE_HAS_CA_PEM)
/** @brief Trusts the ESP-IDF certificate bundle (no CA PEM configured). */
static az_result tls_configure(void* native_config, void* context)
{
  (void)context;
  return esp_crt_bundle_attach(native_config) == ESP_OK ? AZ_OK : AZ_ERROR_NOT_SUPPORTED;
}
#endif

/** @return true if the PUBLISH was acknowledged and the client disconnected cleanly. */
static bool run_sample(void)
{
  az_mqtt_transport* const transport = (az_mqtt_transport*)s_transport.bytes;
  if (az_mqtt_transport_sizeof() > (int32_t)sizeof(s_transport.bytes))
  {
    ESP_LOGE(TAG, "transport storage too small (need %d bytes)", (int)az_mqtt_transport_sizeof());
    return false;
  }
  ESP_LOGI(TAG, "transport storage: %d B", (int)az_mqtt_transport_sizeof());
  az_result rc = az_mqtt_transport_init(transport);
  if (az_result_failed(rc))
  {
    ESP_LOGE(TAG, "transport init failed: 0x%08X", (unsigned)rc);
    return false;
  }

  _V(client_options) options;
  memset(&options, 0, sizeof(options));
#if defined(CONFIG_AZ_MQTT_SAMPLE_USE_TLS)
  static az_mqtt_tls_options tls;
  tls = az_mqtt_tls_options_default();
#if defined(AZ_MQTT_SAMPLE_HAS_CA_PEM)
  tls.ca_cert_pem = az_span_create(
      (uint8_t*)(uintptr_t)az_mqtt_sample_ca_pem_start,
      (int32_t)(az_mqtt_sample_ca_pem_end - az_mqtt_sample_ca_pem_start));
#else
  tls.configure = tls_configure;
#endif
  options.tls_options = &tls;
#endif
  options.transport = transport;
  options.hostname = AZ_SPAN_FROM_STR(CONFIG_AZ_MQTT_SAMPLE_BROKER_HOST);
  options.port = (uint16_t)CONFIG_AZ_MQTT_SAMPLE_BROKER_PORT;
  options.send_buffer = AZ_SPAN_FROM_BUFFER(s_send_buffer);
  options.receive_buffer = AZ_SPAN_FROM_BUFFER(s_receive_buffer);
  options.inflight_control_buffer
      = az_span_create((uint8_t*)s_inflight, (int32_t)sizeof(s_inflight));
  options.connect_options = _V(connect_options_default)();
  static char client_id[sizeof(CONFIG_AZ_MQTT_SAMPLE_CLIENT_ID) + 13];
  uint8_t mac[6];
  if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK)
  {
    ESP_LOGE(TAG, "cannot read the MAC address");
    return false;
  }
  int const id_len = snprintf(
      client_id,
      sizeof(client_id),
      "%s-%02x%02x%02x%02x%02x%02x",
      CONFIG_AZ_MQTT_SAMPLE_CLIENT_ID,
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  options.connect_options.client_id = az_span_create((uint8_t*)client_id, id_len);
  ESP_LOGI(TAG, "client ID %s", client_id);
  options.on_connack = on_connack;
  options.on_puback = on_puback;
  options.on_transport_error = on_transport_error;

  rc = _V(client_init)(&s_client, &options);
  if (az_result_failed(rc))
  {
    ESP_LOGE(TAG, "client init failed: 0x%08X", (unsigned)rc);
    return false;
  }

  int64_t const start_us = esp_timer_get_time();
  rc = _V(client_connect)(&s_client, 30000);
  if (az_result_failed(rc))
  {
    ESP_LOGE(TAG, "connect failed: 0x%08X", (unsigned)rc);
    return false;
  }
  ESP_LOGI(
      TAG,
      "connected to %s:%d in %lld ms",
      CONFIG_AZ_MQTT_SAMPLE_BROKER_HOST,
      CONFIG_AZ_MQTT_SAMPLE_BROKER_PORT,
      (long long)((esp_timer_get_time() - start_us) / 1000));

  _V(publish_options) message = _V(publish_options_default)();
  message.topic = AZ_SPAN_FROM_STR(CONFIG_AZ_MQTT_SAMPLE_TOPIC);
  message.payload = AZ_SPAN_FROM_STR("Hello from ESP32");
  message.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  uint16_t packet_id = 0;
  rc = _V(client_publish)(&s_client, &message, &packet_id);
  ESP_LOGI(TAG, "publish packet_id=%u: 0x%08X", packet_id, (unsigned)rc);

  for (int i = 0; i < 10 && az_result_succeeded(rc) && !s_puback_received; i++)
  {
    rc = _V(client_process_loop)(&s_client, 1000);
  }
  if (az_result_failed(rc))
  {
    ESP_LOGE(TAG, "process_loop failed: 0x%08X", (unsigned)rc);
  }
  else if (!s_acknowledged)
  {
    ESP_LOGE(TAG, "PUBLISH not acknowledged");
  }

  az_result const disconnect_rc = _DISCONNECT(&s_client);
  ESP_LOGI(TAG, "disconnect: 0x%08X", (unsigned)disconnect_rc);
  return az_result_succeeded(rc) && s_acknowledged && az_result_succeeded(disconnect_rc);
}

static void sample_task(void* arg)
{
  (void)arg;
  bool const ok = run_sample();
  ESP_LOGI(
      TAG,
      "%s; stack high-water mark %u B, minimum free heap %u B",
      ok ? "SAMPLE PASSED" : "SAMPLE FAILED",
      (unsigned)uxTaskGetStackHighWaterMark(NULL),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  vTaskDelete(NULL);
}

void app_main(void)
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(example_connect());

  if (xTaskCreate(sample_task, "az_mqtt_sample", SAMPLE_TASK_STACK, NULL, 5, NULL) != pdPASS)
  {
    ESP_LOGE(TAG, "SAMPLE FAILED: cannot create the sample task");
  }
}
