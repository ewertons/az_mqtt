# Copyright (c) az_mqtt5_client contributors. All rights reserved.
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.10)
project(az_mqtt5_client LANGUAGES C)

set(CMAKE_C_STANDARD 99)

# ──────────────────────── Options ─────────────────────────────

option(AZ_MQTT5_BUILD_SAMPLES "Build sample applications" ON)

# ──────────────────────── Azure SDK for C dependency ──────────

# Determine platform for the Azure SDK
if(WIN32)
  set(AZ_PLATFORM_IMPL "WIN32" CACHE STRING "Azure SDK platform")
else()
  set(AZ_PLATFORM_IMPL "POSIX" CACHE STRING "Azure SDK platform")
endif()

# Don't build Azure SDK tests or samples
set(UNIT_TESTING OFF CACHE BOOL "" FORCE)
set(TRANSPORT_PAHO OFF CACHE BOOL "" FORCE)

add_subdirectory(deps/azure-sdk-for-c)

# ──────────────────────── az_mqtt5_client library ────────────────

set(AZ_MQTT5_SOURCES
  src/az_mqtt5_codec.c
  src/az_mqtt5_client.c
)

if(WIN32)
  list(APPEND AZ_MQTT5_SOURCES src/platform/transport_win32.c)
else()
  list(APPEND AZ_MQTT5_SOURCES src/platform/transport_posix.c)
endif()

add_library(az_mqtt5_client ${AZ_MQTT5_SOURCES})

target_include_directories(az_mqtt5_client
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(az_mqtt5_client
  PUBLIC
    az::core
)

# OpenSSL for TLS (optional)
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  target_compile_definitions(az_mqtt5_client PRIVATE AZ_MQTT5_TLS_OPENSSL)
  target_link_libraries(az_mqtt5_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  message(STATUS "az_mqtt5_client: TLS support enabled (OpenSSL)")
else()
  message(STATUS "az_mqtt5_client: TLS support disabled (OpenSSL not found)")
endif()

# Platform-specific link libraries
if(WIN32)
  target_link_libraries(az_mqtt5_client PRIVATE ws2_32)
endif()

add_library(az_mqtt5::client ALIAS az_mqtt5_client)

# ──────────────────────── Compiler warnings ───────────────────

if(MSVC)
  target_compile_options(az_mqtt5_client PRIVATE /W4)
else()
  target_compile_options(az_mqtt5_client PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ──────────────────────── Samples ─────────────────────────────

if(AZ_MQTT5_BUILD_SAMPLES)
  add_subdirectory(samples)
endif()

# ──────────────────────── Tests ───────────────────────────────

option(AZ_MQTT5_BUILD_TESTS "Build tests" ON)
if(AZ_MQTT5_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
