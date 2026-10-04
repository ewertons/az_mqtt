# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Tests of internal modules without I/O; every platform.

foreach(version 3 5)
  if(AZ_MQTT_ENABLE_MQTTV${version})
    set(target az_mqtt${version}_test_codec_ack)
    add_executable(${target} test_codec_ack.c)
    target_include_directories(${target} PRIVATE ${cmocka_SOURCE_DIR}/include)
    target_compile_definitions(${target} PRIVATE AZ_MQTT_TEST_VERSION=${version})
    target_link_libraries(${target} PRIVATE az_mqtt::mqttv${version} cmocka)
    az_mqtt_apply_warnings(${target})
    add_test(NAME ${target} COMMAND ${target})
  endif()
endforeach()
if(AZ_MQTT_ENABLE_PROXY)
  add_executable(az_mqtt_test_http_connect test_http_connect.c)
  target_include_directories(az_mqtt_test_http_connect PRIVATE
    ${cmocka_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/platform)
  target_link_libraries(az_mqtt_test_http_connect PRIVATE az_mqtt::core cmocka)
  az_mqtt_apply_warnings(az_mqtt_test_http_connect)
  add_test(NAME az_mqtt_test_http_connect COMMAND az_mqtt_test_http_connect)

  add_executable(az_mqtt_test_proxy_transport test_proxy_transport.c test_fake_transport.c)
  target_include_directories(az_mqtt_test_proxy_transport PRIVATE
    ${cmocka_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/core ${PROJECT_SOURCE_DIR}/src/platform)
  target_link_libraries(az_mqtt_test_proxy_transport PRIVATE az_mqtt::core cmocka)
  az_mqtt_apply_warnings(az_mqtt_test_proxy_transport)
  add_test(NAME az_mqtt_test_proxy_transport COMMAND az_mqtt_test_proxy_transport)
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
