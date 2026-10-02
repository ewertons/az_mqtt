# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Tests of internal modules without I/O; every platform.
if(AZ_MQTT_ENABLE_PROXY)
  add_executable(az_mqtt_test_http_connect test_http_connect.c)
  target_include_directories(az_mqtt_test_http_connect PRIVATE
    ${cmocka_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/platform)
  target_link_libraries(az_mqtt_test_http_connect PRIVATE az_mqtt::core cmocka)
  az_mqtt_apply_warnings(az_mqtt_test_http_connect)
  add_test(NAME az_mqtt_test_http_connect COMMAND az_mqtt_test_http_connect)
endif()

if(AZ_MQTT_ENABLE_WEBSOCKETS)
  foreach(name websocket websocket_transport)
    add_executable(az_mqtt_test_${name} test_${name}.c test_fake_transport.c)
    target_include_directories(az_mqtt_test_${name} PRIVATE
      ${cmocka_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/core ${PROJECT_SOURCE_DIR}/src/platform)
    target_link_libraries(az_mqtt_test_${name} PRIVATE az_mqtt::core cmocka)
    az_mqtt_apply_warnings(az_mqtt_test_${name})
    add_test(NAME az_mqtt_test_${name} COMMAND az_mqtt_test_${name})
  endforeach()
endif()
