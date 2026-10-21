#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Start a local Mosquitto for the e2e tests: plain MQTT on 1883, TLS on 8883,
# MQTT over WebSockets on 8080 (ws) and 8081 (wss), and TLS on 8884 with a server
# certificate whose Extended Key Usage is clientAuth only (clients must refuse it);
# and TLS servers without MQTT that request (8885) or require (8886) a client certificate.
#
#   eng/ci/start-broker.sh [work-dir]
#
# Mints a CA and a server certificate (SAN localhost, 127.0.0.1) and copies the
# CA to tests/broker/certs/ca.crt, where the e2e tests look for it, with two
# bundles over 64 KiB: ca-bundle.crt (other certificates, then the CA) and
# ca-bundle-without-ca.crt (the same without the CA); and two the CA cannot be
# loaded from: the CA, then a truncated certificate (ca-bundle-truncated.crt) or
# one over 32 KiB of PEM (ca-bundle-oversized.crt). Client certificates there too,
# signed by the CA: client-ec and client-rsa (.crt; key: .key PKCS#8, .sec1.key or
# .pkcs1.key, .encrypted.key with password "secret"), client-chain.crt (a leaf
# under an intermediate CA, then the intermediate; key client-leaf.key), and the
# self-signed client-untrusted (.crt, .key).
# Needs openssl, mosquitto and mosquitto_pub on PATH.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="${1:-${root}/build/broker}"
mkdir -p "${work}"

# Windows (Git Bash): keep "/CN=..." from being rewritten as a path, and give
# mosquitto.exe native paths.
export MSYS_NO_PATHCONV=1
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

w="$(native "${work}")" # every path handed to openssl / mosquitto
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 30 -subj "/CN=az-mqtt-ci-ca" \
  -keyout "${w}/ca.key" -out "${w}/ca.crt" 2>&1 | grep -v '^[.+*]*$' || true
openssl req -newkey rsa:2048 -nodes -sha256 -subj "/CN=localhost" \
  -keyout "${w}/server.key" -out "${w}/server.csr" 2>&1 | grep -v '^[.+*]*$' || true
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' > "${work}/san.cnf"
openssl x509 -req -in "${w}/server.csr" -CA "${w}/ca.crt" -CAkey "${w}/ca.key" \
  -CAcreateserial -days 30 -sha256 -extfile "${w}/san.cnf" -out "${w}/server.crt"
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=clientAuth\n' \
  > "${work}/client-auth.cnf"
openssl x509 -req -in "${w}/server.csr" -CA "${w}/ca.crt" -CAkey "${w}/ca.key" \
  -CAcreateserial -days 30 -sha256 -extfile "${w}/client-auth.cnf" -out "${w}/client-auth.crt"
[ -s "${work}/server.crt" ] && [ -s "${work}/client-auth.crt" ] \
  || { echo "certificate generation failed" >&2; exit 1; }

printf 'extendedKeyUsage=clientAuth\n' > "${work}/client.cnf"
printf 'basicConstraints=critical,CA:true\nkeyUsage=critical,keyCertSign,cRLSign\n' \
  > "${work}/intermediate.cnf"
# client_cert <name> <issuer> <extensions> <genpkey args...>: <name>.key (PKCS#8),
# <name>.<traditional>.key and <name>.crt, signed by <issuer>.
client_cert() {
  local name="$1" issuer="$2" ext="$3"
  shift 3
  openssl genpkey "$@" -out "${w}/${name}.key" 2>/dev/null
  openssl req -new -key "${w}/${name}.key" -subj "/CN=${name}" -out "${w}/${name}.csr"
  openssl x509 -req -in "${w}/${name}.csr" -CA "${w}/${issuer}.crt" -CAkey "${w}/${issuer}.key" \
    -CAcreateserial -days 30 -sha256 -extfile "${w}/${ext}.cnf" -out "${w}/${name}.crt" 2>/dev/null
}
ec=(-algorithm EC -pkeyopt ec_paramgen_curve:P-256)
client_cert client-ec ca client "${ec[@]}"
client_cert client-rsa ca client -algorithm RSA -pkeyopt rsa_keygen_bits:2048
client_cert intermediate ca intermediate "${ec[@]}"
client_cert client-leaf intermediate client "${ec[@]}"
openssl pkey -in "${w}/client-ec.key" -traditional -out "${w}/client-ec.sec1.key"
openssl pkey -in "${w}/client-rsa.key" -traditional -out "${w}/client-rsa.pkcs1.key"
openssl pkcs8 -topk8 -in "${w}/client-ec.key" -passout pass:secret -out "${w}/client-ec.encrypted.key"
cat "${work}/client-leaf.crt" "${work}/intermediate.crt" > "${work}/client-chain.crt"
openssl genpkey "${ec[@]}" -out "${w}/client-untrusted.key" 2>/dev/null
openssl req -x509 -new -key "${w}/client-untrusted.key" -subj "/CN=client-untrusted" -days 30 \
  -sha256 -out "${w}/client-untrusted.crt"
grep -q 'BEGIN EC PRIVATE KEY' "${work}/client-ec.sec1.key" \
  && grep -q 'BEGIN RSA PRIVATE KEY' "${work}/client-rsa.pkcs1.key" \
  && grep -q 'BEGIN ENCRYPTED PRIVATE KEY' "${work}/client-ec.encrypted.key" \
  && [ -s "${work}/client-chain.crt" ] && [ -s "${work}/client-untrusted.crt" ] \
  || { echo "client certificate generation failed" >&2; exit 1; }

mkdir -p "${root}/tests/broker/certs"
cp "${work}/ca.crt" "${root}/tests/broker/certs/ca.crt"
for f in client-ec.crt client-ec.key client-ec.sec1.key client-ec.encrypted.key client-rsa.crt \
  client-rsa.key client-rsa.pkcs1.key client-chain.crt client-leaf.key client-untrusted.crt \
  client-untrusted.key; do
  cp "${work}/${f}" "${root}/tests/broker/certs/${f}"
done
for _ in $(seq 80); do cat "${work}/client-auth.crt"; done \
  > "${root}/tests/broker/certs/ca-bundle-without-ca.crt"
cat "${root}/tests/broker/certs/ca-bundle-without-ca.crt" "${work}/ca.crt" \
  > "${root}/tests/broker/certs/ca-bundle.crt"
{ cat "${work}/ca.crt"; head -c 600 "${work}/client-auth.crt"; } \
  > "${root}/tests/broker/certs/ca-bundle-truncated.crt"
{
  cat "${work}/ca.crt"
  echo "-----BEGIN CERTIFICATE-----"
  for _ in $(seq 640); do printf '%064d\n' 0 | tr 0 A; done
  echo "-----END CERTIFICATE-----"
} > "${root}/tests/broker/certs/ca-bundle-oversized.crt"

cat > "${work}/mosquitto.conf" <<CONF
per_listener_settings false
allow_anonymous true
listener 1883 127.0.0.1
listener 8883 127.0.0.1
cafile ${w}/ca.crt
certfile ${w}/server.crt
keyfile ${w}/server.key
listener 8080 127.0.0.1
protocol websockets
listener 8081 127.0.0.1
protocol websockets
cafile ${w}/ca.crt
certfile ${w}/server.crt
keyfile ${w}/server.key
listener 8884 127.0.0.1
cafile ${w}/ca.crt
certfile ${w}/client-auth.crt
keyfile ${w}/server.key
CONF

# A packaged broker may already own 1883.
if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet mosquitto 2>/dev/null; then
  sudo systemctl stop mosquitto
fi

nohup mosquitto -c "${w}/mosquitto.conf" > "${work}/mosquitto.log" 2>&1 &
# TLS only (no MQTT): 8885 requests a client certificate (-verify), 8886 requires one the CA
# issued (-Verify), over TLS 1.2 so that a refusal ends the handshake (TLS 1.3 refuses after the
# client finished).
tls_server() {
  local port="$1"
  shift
  nohup openssl s_server -accept "${port}" -cert "${w}/server.crt" -key "${w}/server.key" \
    -CAfile "${w}/ca.crt" "$@" -www -quiet < /dev/null > "${work}/s_server_${port}.log" 2>&1 &
}
tls_server 8885 -verify 1
tls_server 8886 -Verify 2 -verify_return_error -tls1_2

for _ in $(seq 1 30); do
  if mosquitto_pub -h 127.0.0.1 -p 1883 -t ci/ready -m ok 2>/dev/null \
    && mosquitto_pub -h localhost -p 8883 --cafile "${w}/ca.crt" -t ci/ready -m ok 2>/dev/null \
    && openssl s_client -connect 127.0.0.1:8885 -brief < /dev/null > /dev/null 2>&1; then
    echo "Mosquitto ready (1883, 8883, 8080, 8081, 8884); openssl s_server (8885, 8886)"
    exit 0
  fi
  sleep 1
done
echo "Mosquitto did not start:" >&2
cat "${work}/mosquitto.log" >&2
exit 1
