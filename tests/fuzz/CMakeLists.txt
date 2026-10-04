# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# libFuzzer targets (AZ_MQTT_BUILD_FUZZERS=ON); see README.md.
if(NOT (AZ_MQTT_ENABLE_PROXY AND AZ_MQTT_ENABLE_WEBSOCKETS))
  message(FATAL_ERROR "The fuzzers need AZ_MQTT_ENABLE_PROXY and AZ_MQTT_ENABLE_WEBSOCKETS")
endif()

set(_fuzz_includes ${PROJECT_SOURCE_DIR}/tests/unit ${PROJECT_SOURCE_DIR}/src/core)

foreach(version 3 5)
  if(AZ_MQTT_ENABLE_MQTTV${version})
    set(target az_mqtt${version}_fuzz_client)
    add_executable(${target} fuzz_client.c ${PROJECT_SOURCE_DIR}/tests/unit/test_fake_transport.c)
    target_include_directories(${target} PRIVATE ${_fuzz_includes})
    target_compile_definitions(${target} PRIVATE AZ_MQTT_FUZZ_VERSION=${version})
    target_link_libraries(${target} PRIVATE az_mqtt::mqttv${version})
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
    az_mqtt_apply_warnings(${target})
  endif()
endforeach()

add_executable(az_mqtt_fuzz_transport
  fuzz_transport.c ${PROJECT_SOURCE_DIR}/tests/unit/test_fake_transport.c)
target_include_directories(az_mqtt_fuzz_transport PRIVATE ${_fuzz_includes})
target_link_libraries(az_mqtt_fuzz_transport PRIVATE az_mqtt::core)
target_link_options(az_mqtt_fuzz_transport PRIVATE -fsanitize=fuzzer)
az_mqtt_apply_warnings(az_mqtt_fuzz_transport)
