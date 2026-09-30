#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Start a local Mosquitto for the e2e tests: plain MQTT on 1883, TLS on 8883.
#
#   eng/ci/start-broker.sh [work-dir]
#
# Mints a CA and a server certificate (SAN localhost, 127.0.0.1) and copies the
# CA to <lib>/tests/broker/certs/ca.crt, where the e2e tests look for it.
# Needs openssl, mosquitto and mosquitto_pub on PATH.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="${1:-${root}/build/broker}"
mkdir -p "${work}"

# Windows (Git Bash): mosquitto.exe wants native paths.
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 30 -subj "/CN=az-mqtt-ci-ca" \
  -keyout "${work}/ca.key" -out "${work}/ca.crt" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -sha256 -subj "/CN=localhost" \
  -keyout "${work}/server.key" -out "${work}/server.csr" 2>/dev/null
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' > "${work}/san.cnf"
openssl x509 -req -in "${work}/server.csr" -CA "${work}/ca.crt" -CAkey "${work}/ca.key" \
  -CAcreateserial -days 30 -sha256 -extfile "${work}/san.cnf" -out "${work}/server.crt" 2>/dev/null

for lib in az_mqtt5 az_mqtt3; do
  mkdir -p "${root}/${lib}/tests/broker/certs"
  cp "${work}/ca.crt" "${root}/${lib}/tests/broker/certs/ca.crt"
done

cat > "${work}/mosquitto.conf" <<CONF
per_listener_settings false
allow_anonymous true
listener 1883 127.0.0.1
listener 8883 127.0.0.1
cafile $(native "${work}/ca.crt")
certfile $(native "${work}/server.crt")
keyfile $(native "${work}/server.key")
CONF

# A packaged broker may already own 1883.
if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet mosquitto 2>/dev/null; then
  sudo systemctl stop mosquitto
fi

nohup mosquitto -c "$(native "${work}/mosquitto.conf")" > "${work}/mosquitto.log" 2>&1 &

for _ in $(seq 1 30); do
  if mosquitto_pub -h 127.0.0.1 -p 1883 -t ci/ready -m ok 2>/dev/null \
    && mosquitto_pub -h localhost -p 8883 --cafile "$(native "${work}/ca.crt")" -t ci/ready -m ok 2>/dev/null; then
    echo "Mosquitto ready (1883, 8883)"
    exit 0
  fi
  sleep 1
done
echo "Mosquitto did not start:" >&2
cat "${work}/mosquitto.log" >&2
exit 1
