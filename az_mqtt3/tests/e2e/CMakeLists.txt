# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

function(add_az_mqtt3_e2e_test target_name source_file)
  add_executable(${target_name}
    ${source_file}
    test_common.c
  )

  target_include_directories(${target_name}
    PRIVATE
      ${cmocka_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}
  )

  target_link_libraries(${target_name}
    PRIVATE
      az_mqtt3::client
      cmocka
  )

  az_mqtt3_apply_warnings(${target_name})
  add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

# ── Integration tests: plain TCP ──────────────────────────────

add_az_mqtt3_e2e_test(az_mqtt3_test_connect test_connect.c)
add_az_mqtt3_e2e_test(az_mqtt3_test_publish test_publish.c)
add_az_mqtt3_e2e_test(az_mqtt3_test_subscribe test_subscribe.c)

# ── Integration tests: TLS (OS-specific) ─────────────────────

if(WIN32)
  add_az_mqtt3_e2e_test(az_mqtt3_test_ssl_win32 test_ssl_win32.c)
  file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/tests/broker/certs/ca.crt" E2E_CA_CERT_PATH)
  target_compile_definitions(
    az_mqtt3_test_ssl_win32
    PRIVATE
      E2E_WIN32_TLS_BACKEND_AVAILABLE=1
      E2E_CA_CERT_PATH="${E2E_CA_CERT_PATH}")
elseif(UNIX)
  # The test uses only the az_mqtt3 API, so it runs on any TLS backend.
  if(AZ_MQTT3_TLS_BACKEND STREQUAL "mbedtls" OR OpenSSL_FOUND)
    file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/tests/broker/certs/ca.crt" E2E_CA_CERT_PATH)
    add_az_mqtt3_e2e_test(az_mqtt3_test_ssl_posix test_ssl_posix.c)
    target_compile_definitions(az_mqtt3_test_ssl_posix PRIVATE E2E_CA_CERT_PATH="${E2E_CA_CERT_PATH}")
  else()
    message(STATUS "Skipping POSIX TLS e2e tests: no TLS backend")
  endif()
endif()
