# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# End-to-end tests against a broker (see eng/ci/start-broker.sh). Each is built
# once per enabled protocol version: az_mqtt<version>_test_<name>.

file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/tests/broker/certs/ca.crt" E2E_CA_CERT_PATH)

# add_az_mqtt_e2e_test(<name> <source> [<compile definition>...])
function(add_az_mqtt_e2e_test name source_file)
  foreach(version 3 5)
    if(NOT AZ_MQTT_ENABLE_MQTT${version})
      continue()
    endif()
    set(target az_mqtt${version}_test_${name})
    add_executable(${target} ${source_file} test_common.c)
    target_include_directories(${target} PRIVATE
      ${cmocka_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests)
    target_compile_definitions(${target} PRIVATE AZ_MQTT_TEST_VERSION=${version} ${ARGN})
    target_link_libraries(${target} PRIVATE az_mqtt::mqtt${version} cmocka)
    az_mqtt_apply_warnings(${target})
    add_test(NAME ${target} COMMAND ${target})
  endforeach()
endfunction()

# ── Plain TCP ────────────────────────────────────────────────

add_az_mqtt_e2e_test(connect test_connect.c)
add_az_mqtt_e2e_test(publish test_publish.c)
add_az_mqtt_e2e_test(subscribe test_subscribe.c)

# ── TLS ──────────────────────────────────────────────────────

if(WIN32)
  add_az_mqtt_e2e_test(ssl_win32 test_ssl_win32.c
    E2E_WIN32_TLS_BACKEND_AVAILABLE=1 E2E_CA_CERT_PATH="${E2E_CA_CERT_PATH}")
elseif(AZ_MQTT_TLS_BACKEND_RESOLVED STREQUAL "none")
  message(STATUS "Skipping TLS e2e tests: no TLS backend")
else()
  # Uses only the library API, so it runs on any TLS backend.
  add_az_mqtt_e2e_test(ssl_posix test_ssl_posix.c E2E_CA_CERT_PATH="${E2E_CA_CERT_PATH}")
endif()
