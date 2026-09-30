# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Transport tests run against an in-process peer (POSIX + OpenSSL), so they
# need no broker and cover every TLS backend the library can be built with.
find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

add_library(az_mqtt5_test_server STATIC ../support/test_server.c)
target_include_directories(az_mqtt5_test_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../support)
target_compile_definitions(az_mqtt5_test_server PRIVATE _DEFAULT_SOURCE)
target_link_libraries(az_mqtt5_test_server PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
az_mqtt5_apply_warnings(az_mqtt5_test_server)

# The library again, with 1 s transport bounds, so timeout paths run in seconds.
list(TRANSFORM AZ_MQTT5_SOURCES PREPEND "${PROJECT_SOURCE_DIR}/" OUTPUT_VARIABLE _sources)
add_library(az_mqtt5_client_short_timeouts STATIC ${_sources})
foreach(_prop INCLUDE_DIRECTORIES COMPILE_DEFINITIONS)
  get_target_property(_value az_mqtt5_client ${_prop})
  if(_value)
    set_property(TARGET az_mqtt5_client_short_timeouts PROPERTY ${_prop} ${_value})
  endif()
endforeach()
get_target_property(_libs az_mqtt5_client LINK_LIBRARIES)
target_link_libraries(az_mqtt5_client_short_timeouts PUBLIC ${_libs})
target_compile_definitions(az_mqtt5_client_short_timeouts PUBLIC
  AZ_MQTT5_TRANSPORT_SEND_TIMEOUT_MS=1000 AZ_MQTT5_TRANSPORT_CONNECT_TIMEOUT_MS=1000)
target_include_directories(az_mqtt5_client_short_timeouts PUBLIC ${PROJECT_SOURCE_DIR}/inc)

string(TOUPPER "${AZ_MQTT5_TLS_BACKEND_RESOLVED}" _backend)

# add_az_mqtt5_transport_test(<target> <source> [<client library>])
function(add_az_mqtt5_transport_test target_name source_file)
  set(_client az_mqtt5::client)
  if(ARGC GREATER 2)
    set(_client ${ARGV2})
  endif()
  add_executable(${target_name} ${source_file})
  target_include_directories(${target_name} PRIVATE
    ${cmocka_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/platform)
  target_compile_definitions(${target_name} PRIVATE AZ_MQTT5_TEST_BACKEND_${_backend} _DEFAULT_SOURCE)
  target_link_libraries(${target_name} PRIVATE ${_client} az_mqtt5_test_server cmocka)
  az_mqtt5_apply_warnings(${target_name})
  add_test(NAME ${target_name} COMMAND ${target_name})
  # A regression here tends to hang rather than fail.
  set_tests_properties(${target_name} PROPERTIES TIMEOUT 120)
endfunction()

add_az_mqtt5_transport_test(az_mqtt5_test_transport_tls test_transport_tls.c)
add_az_mqtt5_transport_test(az_mqtt5_test_transport_robustness test_transport_robustness.c)
add_az_mqtt5_transport_test(az_mqtt5_test_transport_timeouts test_transport_timeouts.c
  az_mqtt5_client_short_timeouts)
