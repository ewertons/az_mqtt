# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
cmake_minimum_required(VERSION 3.10)
project(az_mqtt5_perf LANGUAGES C)
set(CMAKE_C_STANDARD 99)

# ── az_mqtt5 perf binary ──────────────────────────────────────
# This is built against the project tree (assumes the parent project has been
# configured with -DAZ_MQTT5_BUILD_TESTS=ON or the library target is available).
# When built standalone, the library root must be passed via AZ_MQTT5_ROOT.

if(NOT TARGET az_mqtt5::client)
  if(DEFINED AZ_MQTT5_ROOT)
    add_subdirectory(${AZ_MQTT5_ROOT} ${CMAKE_BINARY_DIR}/_az_mqtt5 EXCLUDE_FROM_ALL)
  else()
    # Assume we are inside the project tree at tests/client-comp/
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_BINARY_DIR}/_az_mqtt5 EXCLUDE_FROM_ALL)
  endif()
endif()

add_executable(perf_az_mqtt5 perf_az_mqtt5.c)
target_link_libraries(perf_az_mqtt5 PRIVATE az_mqtt5::client)

if(UNIX)
  target_link_libraries(perf_az_mqtt5 PRIVATE rt)
endif()

# ── Paho MQTT C perf binary ──────────────────────────────────
# Auto-fetch Paho MQTT C from GitHub if not already installed.
include(FetchContent)

find_package(eclipse-paho-mqtt-c QUIET)
if(eclipse-paho-mqtt-c_FOUND)
  message(STATUS "Found system Paho MQTT C")
  add_executable(perf_paho perf_paho.c)
  target_link_libraries(perf_paho PRIVATE eclipse-paho-mqtt-c::paho-mqtt3c)
else()
  message(STATUS "Paho MQTT C not found locally – fetching from GitHub...")
  set(PAHO_WITH_SSL FALSE CACHE BOOL "" FORCE)
  set(PAHO_BUILD_SHARED FALSE CACHE BOOL "" FORCE)
  set(PAHO_BUILD_STATIC TRUE CACHE BOOL "" FORCE)
  set(PAHO_ENABLE_TESTING FALSE CACHE BOOL "" FORCE)
  set(PAHO_BUILD_SAMPLES FALSE CACHE BOOL "" FORCE)
  set(PAHO_BUILD_DOCUMENTATION FALSE CACHE BOOL "" FORCE)
  set(PAHO_HIGH_PERFORMANCE TRUE CACHE BOOL "" FORCE)

  FetchContent_Declare(
    paho_mqtt_c
    GIT_REPOSITORY https://github.com/eclipse-paho/paho.mqtt.c.git
    GIT_TAG        v1.3.14
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(paho_mqtt_c)

  add_executable(perf_paho perf_paho.c)
  target_link_libraries(perf_paho PRIVATE paho-mqtt3a-static)
  target_include_directories(perf_paho PRIVATE ${paho_mqtt_c_SOURCE_DIR}/src)
endif()

if(UNIX)
  target_link_libraries(perf_paho PRIVATE rt)
endif()

# ── Production/size optimization for perf binaries ────────────
# Apply only in Release mode. Goal: minimize on-disk ELF size.
#   -Os                  : optimize for size
#   -ffunction-sections  : place each function in its own section
#   -fdata-sections      : place each data object in its own section
#   -Wl,--gc-sections    : strip unreferenced sections at link time
#   -Wl,-s               : strip all symbol table + relocation info from output
#   -fno-unwind-tables   : drop .eh_frame (no C++ / no backtraces)
#   -fno-asynchronous-unwind-tables
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang"))
  set(_perf_size_flags
      -Os
      -ffunction-sections
      -fdata-sections
      -fno-unwind-tables
      -fno-asynchronous-unwind-tables
  )
  set(_perf_size_link_flags
      -Wl,--gc-sections
      -Wl,-s
  )
  foreach(_tgt perf_az_mqtt5 perf_paho)
    target_compile_options(${_tgt} PRIVATE ${_perf_size_flags})
    target_link_options(${_tgt} PRIVATE ${_perf_size_link_flags})
  endforeach()
endif()
