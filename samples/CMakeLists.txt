# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Samples per protocol version; each links only its own library, plus the shared settings module.
add_library(az_mqtt_sample_common STATIC az_mqtt_sample_common.c)
target_include_directories(az_mqtt_sample_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(az_mqtt_sample_common PUBLIC az::core)
az_mqtt_apply_warnings(az_mqtt_sample_common)

set(_samples connect tls nonblocking)
if(AZ_MQTT_ENABLE_WEBSOCKETS)
  list(APPEND _samples websocket)
endif()

foreach(version 3 5)
  if(NOT AZ_MQTT_ENABLE_MQTTV${version})
    continue()
  endif()
  set(_version_samples ${_samples})
  if(version EQUAL 5)
    list(APPEND _version_samples request_response)
  endif()
  foreach(sample ${_version_samples})
    set(target az_mqtt${version}_sample_${sample})
    add_executable(${target} ${target}.c)
    target_link_libraries(${target} PRIVATE az_mqtt::mqttv${version} az_mqtt_sample_common)
    az_mqtt_apply_warnings(${target})
  endforeach()
endforeach()
