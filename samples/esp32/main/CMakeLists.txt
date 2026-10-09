# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

idf_component_register(
  SRCS "app_main.c"
  PRIV_REQUIRES az_mqtt protocol_examples_common nvs_flash esp_netif esp_event esp_timer mbedtls)

if(CONFIG_AZ_MQTT_SAMPLE_USE_TLS AND NOT CONFIG_AZ_MQTT_SAMPLE_CA_PEM_FILE STREQUAL "")
  # The CA path is relative to the project directory.
  target_add_binary_data(${COMPONENT_LIB} "${PROJECT_DIR}/${CONFIG_AZ_MQTT_SAMPLE_CA_PEM_FILE}" TEXT
    RENAME_TO az_mqtt_sample_ca_pem)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE AZ_MQTT_SAMPLE_HAS_CA_PEM)
endif()
