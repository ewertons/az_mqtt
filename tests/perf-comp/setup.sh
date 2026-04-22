#!/usr/bin/env bash
# setup.sh – Install dependencies, build perf binaries, and start infrastructure.
# Run from the test/perf-conf/ directory.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/results"

echo "=== [1/4] Installing build dependencies ==="
if command -v apt-get &>/dev/null; then
  sudo apt-get update -qq
  sudo apt-get install -y -qq \
    build-essential cmake libssl-dev \
    libpaho-mqtt-dev docker-compose-plugin jq
elif command -v dnf &>/dev/null; then
  sudo dnf install -y gcc cmake openssl-devel \
    paho-c-devel docker-compose jq
else
  echo "WARN: Unknown package manager – ensure cmake, libssl-dev, libpaho-mqtt-dev, docker compose, jq are installed."
fi

echo "=== [2/4] Building perf binaries ==="
BUILD_DIR="${SCRIPT_DIR}/build"
mkdir -p "${BUILD_DIR}"
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --config Release -j "$(nproc)"

echo "=== [3/4] Starting infrastructure (EMQX + Prometheus + Grafana + cAdvisor) ==="
docker compose -f "${SCRIPT_DIR}/docker-compose.yml" up -d

echo "Waiting for EMQX to become healthy..."
for i in $(seq 1 30); do
  if docker exec perf-emqx emqx ping 2>/dev/null | grep -q pong; then
    echo "EMQX is ready."
    break
  fi
  sleep 2
done

mkdir -p "${RESULTS_DIR}"

echo "=== [4/4] Setup complete ==="
echo "  Binaries : ${BUILD_DIR}/perf_az_mqtt5  ${BUILD_DIR}/perf_paho"
echo "  Results  : ${RESULTS_DIR}/"
echo "  EMQX     : mqtt://localhost:1883  dashboard http://localhost:18083"
echo "  Prometheus : http://localhost:9090"
echo "  Grafana  : http://localhost:3000  (admin/admin)"
echo ""
echo "Next: run  ./run_az_mqtt5.sh  and  ./run_paho.sh"
