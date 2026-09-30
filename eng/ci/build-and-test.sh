#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Configure, build (warnings are errors) and test one library/backend.
#
#   eng/ci/build-and-test.sh <az_mqtt5|az_mqtt3> <openssl|mbedtls|none> [profile]
#
# profile: debug (default), asan (ASan + UBSan), hardened (release with
# fortify, stack protector, CET, full RELRO; binaries are checked afterwards).
# Environment: CC, MBEDTLS_PREFIX (for mbedtls), CMAKE_ARGS (extra, e.g. a
# local cmocka via -DFETCHCONTENT_SOURCE_DIR_CMOCKA=...). E2E tests need
# eng/ci/start-broker.sh first.
set -euo pipefail

[ "$#" -ge 2 ] || { echo "usage: ${0##*/} <az_mqtt5|az_mqtt3> <openssl|mbedtls|none> [debug|asan|hardened]" >&2; exit 1; }
lib="$1"
tls="$2"
profile="${3:-debug}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
opt="$(echo "${lib#az_}" | tr '[:lower:]' '[:upper:]')" # MQTT5 / MQTT3
build="${root}/build/${lib}-${tls}-${profile}"

build_type=Debug
c_flags=""
link_flags=""
case "${profile}" in
  debug) ;;
  asan)
    c_flags="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
    link_flags="-fsanitize=address,undefined"
    ;;
  hardened)
    build_type=Release
    c_flags="-O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fcf-protection -fPIE"
    link_flags="-pie -Wl,-z,relro,-z,now,-z,noexecstack"
    ;;
  *) echo "unknown profile ${profile}" >&2; exit 1 ;;
esac

args=(
  -S "${root}/${lib}" -B "${build}" -G Ninja
  -DCMAKE_BUILD_TYPE="${build_type}"
  -D"AZ_${opt}_BUILD_TESTS=ON"
  -D"AZ_${opt}_BUILD_SAMPLES=ON"
  -D"AZ_${opt}_TLS_BACKEND=${tls}"
  -D"AZ_${opt}_WARNINGS_AS_ERRORS=ON"
  -DCMAKE_C_FLAGS="${c_flags}"
  -DCMAKE_EXE_LINKER_FLAGS="${link_flags}"
)
if [ "${tls}" = mbedtls ]; then
  args+=(-DCMAKE_PREFIX_PATH="${MBEDTLS_PREFIX:?set MBEDTLS_PREFIX}")
fi
# shellcheck disable=SC2206
args+=(${CMAKE_ARGS:-})

cmake "${args[@]}"
cmake --build "${build}"

if [ "${profile}" = hardened ]; then
  # Every executable we link must be PIE with full RELRO and a non-executable stack.
  status=0
  while IFS= read -r exe; do
    hdr="$(readelf -h -l -d "${exe}")"
    for need in "Type: *DYN" "GNU_RELRO" "BIND_NOW" ; do
      grep -Eq "${need}" <<<"${hdr}" || { echo "${exe}: missing ${need}" >&2; status=1; }
    done
    if grep -A1 GNU_STACK <<<"${hdr}" | grep -q RWE; then
      echo "${exe}: executable stack" >&2; status=1
    fi
  done < <(find "${build}" -path "${build}/_deps" -prune -o -type f -perm -u+x -name "az_mqtt*" -print)
  [ "${status}" -eq 0 ] || exit 1
  echo "Hardening checks passed"
fi

ctest --test-dir "${build}" --output-on-failure --timeout 300
