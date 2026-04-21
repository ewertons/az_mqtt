# Copyright (c) az_mqtt5_client contributors. All rights reserved.
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.14)

# ── Fetch cmocka via FetchContent ────────────────────────────
include(FetchContent)

FetchContent_Declare(
  cmocka
  GIT_REPOSITORY https://gitlab.com/cmocka/cmocka.git
  GIT_TAG        cmocka-1.1.7
)

# Force cmocka to build as a static library to avoid cyclic dependency issues
set(_saved_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_STATIC_LIB ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(cmocka)

# Restore BUILD_SHARED_LIBS
set(BUILD_SHARED_LIBS ${_saved_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)

# ── Integration test: connect/disconnect ─────────────────────

add_executable(az_mqtt5_test_connect
  test_connect.c
)

target_include_directories(az_mqtt5_test_connect
  PRIVATE
    ${cmocka_SOURCE_DIR}/include
)

target_link_libraries(az_mqtt5_test_connect
  PRIVATE
    az_mqtt5::client
    cmocka
)

add_test(NAME az_mqtt5_test_connect COMMAND az_mqtt5_test_connect)
