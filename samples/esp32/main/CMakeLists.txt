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

if(NOT CONFIG_AZ_MQTT_BUILD_AZ_CORE)
  # az_mqtt then expects the application to provide the azure-sdk-for-c sources it uses.
  set(_az_core "${CMAKE_CURRENT_LIST_DIR}/../../../deps/azure-sdk-for-c/sdk/src/azure/core")
  target_sources(${COMPONENT_LIB} PRIVATE
    "${_az_core}/az_span.c" "${_az_core}/az_log.c" "${_az_core}/az_precondition.c")
  if(CONFIG_AZ_MQTT_ENABLE_PROXY OR CONFIG_AZ_MQTT_ENABLE_WEBSOCKETS)
    target_sources(${COMPONENT_LIB} PRIVATE "${_az_core}/az_base64.c")
  endif()
endif()
