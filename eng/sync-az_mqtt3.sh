#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# az_mqtt3 shares everything but its codec with az_mqtt5. This regenerates the
# shared az_mqtt3 files from az_mqtt5 so a fix is made once.
#
#   eng/sync-az_mqtt3.sh          regenerate
#   eng/sync-az_mqtt3.sh --check  fail if az_mqtt3 is out of sync
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
check=0
[ "${1:-}" = "--check" ] && check=1

# Relative to az_mqtt5; the codec, samples, docs and build files stay per-variant.
shared=(
  inc/az_mqtt5/az_mqtt5_client.h
  inc/az_mqtt5/az_mqtt5_transport.h
  inc/az_mqtt5/az_mqtt5_types.h
  src/az_mqtt5_client.c
)
while IFS= read -r f; do shared+=("${f#"${root}/az_mqtt5/"}"); done < <(
  find "${root}/az_mqtt5/src/platform" "${root}/az_mqtt5/tests" \
    \( -path '*/client-comp' -o -path '*/broker' \) -prune -o \
    -type f \( -name '*.c' -o -name '*.h' -o -name 'CMakeLists.txt' \) -print | sort)

transform() {
  sed -e 's/mqtt5/mqtt3/g' -e 's/MQTT5/MQTT3/g' -e 's/MQTT 5\.0/MQTT 3.1.1/g' "$1"
}

stale=0
for rel in "${shared[@]}"; do
  src="${root}/az_mqtt5/${rel}"
  dst="${root}/az_mqtt3/${rel//mqtt5/mqtt3}"
  if [ "${check}" -eq 1 ]; then
    if ! transform "${src}" | cmp -s - "${dst}" 2>/dev/null; then
      echo "out of sync: az_mqtt3/${rel//mqtt5/mqtt3}"
      stale=1
    fi
  else
    mkdir -p "$(dirname "${dst}")"
    transform "${src}" > "${dst}"
  fi
done
if [ "${check}" -eq 1 ] && [ "${stale}" -ne 0 ]; then
  echo "Run eng/sync-az_mqtt3.sh and commit the result." >&2
  exit 1
fi
