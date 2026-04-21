# Copyright (c) mqtt5_client contributors. All rights reserved.
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.10)

add_executable(mqtt5_sample_connect
  mqtt5_sample_connect.c
)

target_link_libraries(mqtt5_sample_connect
  PRIVATE
    mqtt5::client
)
