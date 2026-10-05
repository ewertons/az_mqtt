#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Build az_mqtt as a subproject of tests/subproject, with the parent's azure-sdk-for-c and with
# the bundled one, and run the linked application. Environment: CC, CMAKE_ARGS (extra).

set -euo pipefail
cd "$(dirname "$0")/../.."

for external in ON OFF; do
  build="build/subproject-${external}"
  echo "── az_core from the parent: ${external}"
  rm -rf "${build}"
  # shellcheck disable=SC2086 # CMAKE_ARGS is a list.
  cmake -S tests/subproject -B "${build}" -G Ninja -DAZ_MQTT_TEST_EXTERNAL_AZ_CORE="${external}" \
    -DAZ_MQTT_WARNINGS_AS_ERRORS=ON ${CMAKE_ARGS:-}
  cmake --build "${build}"
  "${build}/az_mqtt_subproject_app"
done
echo "subproject: OK"
