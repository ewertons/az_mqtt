# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# az_mqtt on ESP-IDF: idf.py set-target esp32 && idf.py menuconfig && idf.py build flash monitor

cmake_minimum_required(VERSION 3.16)

# The repository root is the az_mqtt component; ESP-IDF names a component after its directory.
get_filename_component(_az_mqtt_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(_az_mqtt_dir_name "${_az_mqtt_root}" NAME)
if(NOT _az_mqtt_dir_name STREQUAL "az_mqtt")
  message(FATAL_ERROR "The repository directory must be named az_mqtt (got ${_az_mqtt_dir_name})")
endif()
get_filename_component(_az_mqtt_parent "${_az_mqtt_root}" DIRECTORY)

# protocol_examples_common: Wi-Fi or Ethernet (including QEMU's OpenCores Ethernet) via menuconfig.
set(EXTRA_COMPONENT_DIRS
  "${_az_mqtt_root}"
  "$ENV{IDF_PATH}/examples/common_components/protocol_examples_common")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(az_mqtt_esp32_sample)
