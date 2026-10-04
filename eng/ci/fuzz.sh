#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Build the libFuzzer targets and run each for FUZZ_SECONDS (default 60) from its seed corpus.
# Fails on the first crash, leak, timeout or sanitizer report; its input is kept in
# build/fuzz/artifacts. Environment: CC (a clang), FUZZ_SECONDS, CMAKE_ARGS (extra, unquoted).

set -euo pipefail
cd "$(dirname "$0")/../.."
build=build/fuzz
seconds="${FUZZ_SECONDS:-60}"

# shellcheck disable=SC2086 # CMAKE_ARGS is a list.
cmake -S . -B "$build" -G Ninja -DCMAKE_C_COMPILER="${CC:-clang}" -DAZ_MQTT_BUILD_FUZZERS=ON \
  -DAZ_MQTT_BUILD_TESTS=OFF -DAZ_MQTT_BUILD_SAMPLES=OFF -DAZ_MQTT_WARNINGS_AS_ERRORS=ON \
  -DAZ_MQTT_TLS_BACKEND=none ${CMAKE_ARGS:-}
cmake --build "$build"

mkdir -p "$build/artifacts"
for pair in az_mqtt3_fuzz_client:client3 az_mqtt5_fuzz_client:client5 az_mqtt_fuzz_transport:transport; do
  target="${pair%%:*}"
  corpus="${pair#*:}"
  mkdir -p "$build/corpus/$corpus"
  echo "── $target: ${seconds} s"
  # The first directory collects new inputs; the seed corpus is only read.
  "$build/tests/fuzz/$target" -max_total_time="$seconds" -max_len=4096 -timeout=10 \
    -rss_limit_mb=2048 -print_final_stats=1 -artifact_prefix="$build/artifacts/$target-" \
    "$build/corpus/$corpus" "tests/fuzz/corpus/$corpus"
done
