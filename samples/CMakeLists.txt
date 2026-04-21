# Copyright (c) az_mqtt5_client contributors. All rights reserved.
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.10)

add_executable(az_mqtt5_sample_connect
  az_mqtt5_sample_connect.c
)

target_link_libraries(az_mqtt5_sample_connect
  PRIVATE
    az_mqtt5::client
)
