# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 3.10)

add_executable(az_mqtt5_sample_connect
  az_mqtt5_sample_connect.c
)

target_link_libraries(az_mqtt5_sample_connect
  PRIVATE
    az_mqtt5::client
)

az_mqtt5_apply_warnings(az_mqtt5_sample_connect)
