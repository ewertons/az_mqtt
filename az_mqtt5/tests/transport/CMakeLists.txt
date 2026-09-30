# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Transport tests run against an in-process peer (POSIX + OpenSSL), so they
# need no broker and cover every TLS backend the library can be built with.
find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

add_library(az_mqtt5_test_server STATIC ../support/test_server.c)
target_include_directories(az_mqtt5_test_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../support)
target_compile_definitions(az_mqtt5_test_server PRIVATE _GNU_SOURCE)
target_link_libraries(az_mqtt5_test_server PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
az_mqtt5_apply_warnings(az_mqtt5_test_server)

string(TOUPPER "${AZ_MQTT5_TLS_BACKEND_RESOLVED}" _backend)

function(add_az_mqtt5_transport_test target_name source_file)
  add_executable(${target_name} ${source_file})
  target_include_directories(${target_name} PRIVATE ${cmocka_SOURCE_DIR}/include)
  target_compile_definitions(${target_name} PRIVATE AZ_MQTT5_TEST_BACKEND_${_backend})
  target_link_libraries(${target_name} PRIVATE az_mqtt5::client az_mqtt5_test_server cmocka)
  az_mqtt5_apply_warnings(${target_name})
  add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

add_az_mqtt5_transport_test(az_mqtt5_test_transport_tls test_transport_tls.c)
