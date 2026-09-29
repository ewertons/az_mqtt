# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# In-process peer (POSIX + OpenSSL) for tests that need no broker.
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(az_mqtt3_test_server STATIC test_server.c)
target_include_directories(az_mqtt3_test_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(az_mqtt3_test_server PRIVATE _DEFAULT_SOURCE)
target_link_libraries(az_mqtt3_test_server PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

string(TOUPPER "${AZ_MQTT3_TLS_BACKEND_RESOLVED}" _backend)
set(AZ_MQTT3_TEST_BACKEND_DEFINE "AZ_MQTT3_TEST_BACKEND_${_backend}" PARENT_SCOPE)

# add_az_mqtt3_local_test(<target> <source>): a cmocka test against the in-process peer.
function(add_az_mqtt3_local_test target_name source_file)
  add_executable(${target_name} ${source_file})
  target_include_directories(${target_name} PRIVATE ${cmocka_SOURCE_DIR}/include)
  target_compile_definitions(${target_name} PRIVATE ${AZ_MQTT3_TEST_BACKEND_DEFINE} _DEFAULT_SOURCE)
  target_link_libraries(${target_name} PRIVATE az_mqtt3::client az_mqtt3_test_server cmocka)
  add_test(NAME ${target_name} COMMAND ${target_name})
  # A regression here tends to hang rather than fail.
  set_tests_properties(${target_name} PROPERTIES TIMEOUT 120)
endfunction()
