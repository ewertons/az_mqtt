# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

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

# TLS backend selection. Valid values: auto (default), openssl, mbedtls, none.
#   auto   — use OpenSSL if found, otherwise no TLS.
#   openssl — require OpenSSL; compile transport_posix.c with OpenSSL.
#   mbedtls — require mbedTLS; compile transport_mbedtls.c.
#   none   — compile transport_posix.c without any TLS support.
set(AZ_MQTT5_TLS_BACKEND "auto" CACHE STRING "TLS backend: auto | openssl | mbedtls | none")

if(WIN32)
  list(APPEND AZ_MQTT5_SOURCES src/platform/transport_win32.c)
elseif(AZ_MQTT5_TLS_BACKEND STREQUAL "mbedtls")
  list(APPEND AZ_MQTT5_SOURCES src/platform/transport_mbedtls.c src/platform/az_mqtt5_socket_posix.c)
else()
  list(APPEND AZ_MQTT5_SOURCES src/platform/transport_posix.c src/platform/az_mqtt5_socket_posix.c)
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

# Wire the selected TLS backend.
if(WIN32)
  target_compile_definitions(az_mqtt5_client PRIVATE AZ_MQTT5_TLS_SCHANNEL)
  target_link_libraries(az_mqtt5_client PRIVATE secur32 crypt32)
  set(AZ_MQTT5_TLS_BACKEND_RESOLVED "schannel")
  message(STATUS "az_mqtt5_client: TLS backend = Schannel")
elseif(AZ_MQTT5_TLS_BACKEND STREQUAL "mbedtls")
  find_library(MBEDTLS_SSL_LIB    NAMES mbedtls    REQUIRED)
  find_library(MBEDTLS_X509_LIB   NAMES mbedx509   REQUIRED)
  find_library(MBEDTLS_CRYPTO_LIB NAMES mbedcrypto REQUIRED)
  find_path(MBEDTLS_INCLUDE_DIR NAMES mbedtls/ssl.h REQUIRED)
  target_include_directories(az_mqtt5_client PRIVATE ${MBEDTLS_INCLUDE_DIR})
  target_compile_definitions(az_mqtt5_client PRIVATE AZ_MQTT5_TLS_MBEDTLS)
  set(AZ_MQTT5_TLS_BACKEND_RESOLVED "mbedtls")
  # Order matters: ssl depends on x509 depends on crypto.
  target_link_libraries(az_mqtt5_client PRIVATE
      ${MBEDTLS_SSL_LIB} ${MBEDTLS_X509_LIB} ${MBEDTLS_CRYPTO_LIB})
  message(STATUS "az_mqtt5_client: TLS backend = mbedTLS")
elseif(AZ_MQTT5_TLS_BACKEND STREQUAL "openssl"
  OR AZ_MQTT5_TLS_BACKEND STREQUAL "auto")
  if(AZ_MQTT5_TLS_BACKEND STREQUAL "openssl")
    find_package(OpenSSL REQUIRED)
  else()
    find_package(OpenSSL QUIET)
  endif()
  if(OpenSSL_FOUND)
    target_compile_definitions(az_mqtt5_client PRIVATE AZ_MQTT5_TLS_OPENSSL)
    target_link_libraries(az_mqtt5_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    set(AZ_MQTT5_TLS_BACKEND_RESOLVED "openssl")
    message(STATUS "az_mqtt5_client: TLS backend = OpenSSL")
  else()
    # TLS connects fail with AZ_MQTT5_ERROR_NOT_SUPPORTED; they never fall back to plaintext.
    message(WARNING "az_mqtt5_client: TLS support disabled (OpenSSL not found)")
    set(AZ_MQTT5_TLS_BACKEND_RESOLVED "none")
  endif()
else()
  message(STATUS "az_mqtt5_client: TLS support disabled (backend = none)")
  set(AZ_MQTT5_TLS_BACKEND_RESOLVED "none")
endif()

# Platform-specific link libraries
if(WIN32)
  target_link_libraries(az_mqtt5_client PRIVATE ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(az_mqtt5_client PRIVATE Threads::Threads)
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
