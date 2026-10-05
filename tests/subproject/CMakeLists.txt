# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# A parent project that embeds az_mqtt with add_subdirectory(); see eng/ci/subproject.sh.
#   AZ_MQTT_TEST_EXTERNAL_AZ_CORE=ON: the parent builds azure-sdk-for-c itself (az_core exists
#     before az_mqtt is added); OFF: az_mqtt builds its bundled copy.
# Configure fails if az_mqtt changed any of the parent's cache entries, or built samples or tests.

cmake_minimum_required(VERSION 3.14)
project(az_mqtt_subproject_test LANGUAGES C)

option(AZ_MQTT_TEST_EXTERNAL_AZ_CORE "The parent builds azure-sdk-for-c" OFF)
set(_az_mqtt_root ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Parent-owned cache entries that az_mqtt must not change.
set(UNIT_TESTING ON CACHE BOOL "The parent's own setting")
set(TRANSPORT_PAHO ON CACHE BOOL "The parent's own setting")
set(_watched UNIT_TESTING TRANSPORT_PAHO AZ_PLATFORM_IMPL)

if(AZ_MQTT_TEST_EXTERNAL_AZ_CORE)
  # The parent's own azure-sdk-for-c, without its tests and samples (as a parent would do).
  set(AZ_PLATFORM_IMPL "POSIX" CACHE STRING "The parent's own setting")
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(UNIT_TESTING OFF)
  set(TRANSPORT_PAHO OFF)
  add_subdirectory(${_az_mqtt_root}/deps/azure-sdk-for-c azure-sdk-for-c)
  unset(UNIT_TESTING)
  unset(TRANSPORT_PAHO)
  unset(CMAKE_POLICY_DEFAULT_CMP0077)
endif()

foreach(_v IN LISTS _watched)
  get_property(_before_${_v} CACHE ${_v} PROPERTY VALUE)
endforeach()

add_subdirectory(${_az_mqtt_root} az_mqtt)

foreach(_v IN LISTS _watched)
  get_property(_after CACHE ${_v} PROPERTY VALUE)
  if(NOT "${_after}" STREQUAL "${_before_${_v}}")
    message(FATAL_ERROR "az_mqtt changed the parent's cache: ${_v} '${_before_${_v}}' -> '${_after}'")
  endif()
endforeach()
foreach(_t IN ITEMS az_mqtt5_sample_connect az_mqtt5_test_connect az_mqtt_test_websocket cmocka)
  if(TARGET ${_t})
    message(FATAL_ERROR "az_mqtt built ${_t} as a subproject")
  endif()
endforeach()

add_executable(az_mqtt_subproject_app app.c)
target_link_libraries(az_mqtt_subproject_app PRIVATE az_mqtt::mqttv3 az_mqtt::mqttv5)

# The parent's own target linking az_core: az_mqtt must not have made its headers system headers.
get_target_property(_system az_core INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
get_target_property(_includes az_core INTERFACE_INCLUDE_DIRECTORIES)
if(_system AND _includes)
  foreach(_d IN LISTS _includes)
    if(_d IN_LIST _system AND AZ_MQTT_TEST_EXTERNAL_AZ_CORE)
      message(FATAL_ERROR "az_mqtt marked az_core include directory '${_d}' as system")
    endif()
  endforeach()
endif()
