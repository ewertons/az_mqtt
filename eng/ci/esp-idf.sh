#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Build the ESP32 sample (samples/esp32) as an ESP-IDF component user, and optionally run it in
# QEMU against a local Mosquitto.
#
#   eng/ci/esp-idf.sh build          # every samples/esp32/ci/sdkconfig.ci.* variant
#   eng/ci/esp-idf.sh qemu           # mqttv3 and mqttv5 over TLS, mqttv3 over TCP, and a
#                                    # connection that must fail server verification
#
# Needs an ESP-IDF environment (export.sh; IDF_PATH set). qemu also needs qemu-system-xtensa
# (idf_tools.py install qemu-xtensa), openssl and mosquitto on PATH.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
sample="${root}/samples/esp32"
out="${root}/build/esp-idf"
mode="${1:-build}"
mkdir -p "${out}"

# Builds variant $1 with sdkconfig fragments $2... into ${out}/$1.
build_variant() {
  local name="$1"
  shift
  local defaults
  defaults="$(IFS=';'; echo "$*")"
  echo "::group::build ${name}"
  idf.py -C "${sample}" -B "${out}/${name}" -D SDKCONFIG="${out}/${name}/sdkconfig" \
    -D SDKCONFIG_DEFAULTS="${defaults}" build
  echo "::endgroup::"
}

if [ "${mode}" = "build" ]; then
  for cfg in "${sample}"/ci/sdkconfig.ci.*; do
    build_variant "${cfg##*.}" "${cfg}"
  done
  exit 0
fi

[ "${mode}" = "qemu" ] || { echo "usage: $0 build|qemu" >&2; exit 2; }

# export.sh may leave the optional QEMU tool off PATH.
if ! command -v qemu-system-xtensa > /dev/null; then
  qemu="$(find "${IDF_TOOLS_PATH:-${HOME}/.espressif}/tools/qemu-xtensa" -name qemu-system-xtensa \
    -type f 2>/dev/null | head -n 1)"
  [ -n "${qemu}" ] || { echo "qemu-system-xtensa not found (idf_tools.py install qemu-xtensa)" >&2; exit 1; }
  PATH="$(dirname "${qemu}"):${PATH}"
fi

# The guest reaches the host's 127.0.0.1 as 10.0.2.2, so the server certificate names that IP.
work="${out}/broker"
mkdir -p "${work}"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 \
  -subj "/CN=az-mqtt-esp-idf-ca" -keyout "${work}/ca.key" -out "${work}/ca.pem" 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 \
  -subj "/CN=az-mqtt-esp-idf-other-ca" -keyout "${work}/other.key" -out "${work}/other.pem" 2>/dev/null
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -subj "/CN=10.0.2.2" \
  -keyout "${work}/server.key" -out "${work}/server.csr" 2>/dev/null
printf 'subjectAltName=IP:10.0.2.2\n' > "${work}/san.cnf"
openssl x509 -req -in "${work}/server.csr" -CA "${work}/ca.pem" -CAkey "${work}/ca.key" \
  -CAcreateserial -days 2 -extfile "${work}/san.cnf" -out "${work}/server.pem" 2>/dev/null
cat > "${work}/mosquitto.conf" <<EOF
per_listener_settings true
listener 1883 127.0.0.1
allow_anonymous true
listener 8883 127.0.0.1
allow_anonymous true
cafile ${work}/ca.pem
certfile ${work}/server.pem
keyfile ${work}/server.key
EOF
# As root (e.g. in the espressif/idf container), Mosquitto otherwise switches to a user that
# cannot read the certificates.
if [ "$(id -u)" = "0" ]; then
  echo "user root" >> "${work}/mosquitto.conf"
fi
mosquitto -c "${work}/mosquitto.conf" > "${work}/mosquitto.log" 2>&1 &
broker=$!
trap 'kill ${broker} 2>/dev/null || true' EXIT
sleep 1
if ! kill -0 "${broker}" 2>/dev/null; then
  echo "Mosquitto did not start:" >&2
  cat "${work}/mosquitto.log" >&2
  exit 1
fi

# The CA path in sdkconfig is relative to the sample project.
cp "${work}/ca.pem" "${sample}/ci_ca.pem"
cp "${work}/other.pem" "${sample}/ci_other_ca.pem"
printf 'CONFIG_AZ_MQTT_SAMPLE_CA_PEM_FILE="ci_ca.pem"\n' > "${work}/ca.cfg"
printf 'CONFIG_AZ_MQTT_SAMPLE_CA_PEM_FILE="ci_other_ca.pem"\n' > "${work}/other_ca.cfg"
qemu_cfg="${sample}/ci/sdkconfig.qemu"

# Runs variant $1 in QEMU; passes if its log has $2.
run_variant() {
  local name="$1" expect="$2"
  (cd "${out}/${name}" && python -m esptool --chip esp32 merge-bin --fill-flash-size 4MB \
    -o flash.bin @flash_args > /dev/null)
  timeout 60 qemu-system-xtensa -nographic -M esp32 -m 4M \
    -drive "file=${out}/${name}/flash.bin,if=mtd,format=raw" -nic user,model=open_eth \
    > "${out}/${name}/qemu.log" 2>&1 || true
  grep "az_mqtt_sample" "${out}/${name}/qemu.log" || true
  if grep -q "${expect}" "${out}/${name}/qemu.log"; then
    echo "PASS ${name}"
  else
    echo "FAIL ${name} (expected: ${expect}); broker log:"
    tail -n 20 "${work}/mosquitto.log"
    return 1
  fi
}

status=0
build_variant qemu_v3 "${qemu_cfg}" "${work}/ca.cfg"
build_variant qemu_v5 "${qemu_cfg}" "${work}/ca.cfg" "${sample}/ci/sdkconfig.ci.v5"
build_variant qemu_v3_tcp "${qemu_cfg}" "${sample}/ci/sdkconfig.ci.v3_tcp"
build_variant qemu_untrusted "${qemu_cfg}" "${work}/other_ca.cfg"
rm -f "${sample}/ci_ca.pem" "${sample}/ci_other_ca.pem"
run_variant qemu_v3 "SAMPLE PASSED" || status=1
run_variant qemu_v5 "SAMPLE PASSED" || status=1
run_variant qemu_v3_tcp "SAMPLE PASSED" || status=1
# AZ_MQTT_ERROR_TLS_VERIFY
run_variant qemu_untrusted "connect failed: 0x8006001A" || status=1
exit "${status}"
