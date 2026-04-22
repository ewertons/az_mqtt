# Copyright (c) az_mqtt5_client contributors. All rights reserved.
# SPDX-License-Identifier: MIT

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
