#!/usr/bin/env bash
# setup.sh – Install dependencies, build perf binaries, and start infrastructure.
# Run from the tests/perf-comp/ directory.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/results"

echo "=== [1/5] Installing build dependencies ==="
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

# Ensure Rust toolchain is available
if ! command -v cargo &>/dev/null; then
  echo "Installing Rust toolchain..."
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
  source "$HOME/.cargo/env"
fi

echo "=== [2/5] Building C perf binaries ==="
BUILD_DIR="${SCRIPT_DIR}/build"
mkdir -p "${BUILD_DIR}"
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --config Release -j "$(nproc)"

echo "=== [3/5] Cloning and building azure_mqtt (Rust) perf binary ==="
RUST_PERF_DIR="${SCRIPT_DIR}/perf_azure_mqtt"
AZURE_MQTT_SRC="${RUST_PERF_DIR}/azure_mqtt_src"

if [ ! -d "${AZURE_MQTT_SRC}/.git" ]; then
  echo "Cloning https://github.com/Azure/mqtt-client..."
  git clone --depth 1 https://github.com/Azure/mqtt-client "${AZURE_MQTT_SRC}"
else
  echo "azure_mqtt source already present, pulling latest..."
  git -C "${AZURE_MQTT_SRC}" pull --ff-only || true
fi

cargo build --release --manifest-path "${RUST_PERF_DIR}/Cargo.toml"

echo "=== [4/5] Starting infrastructure (EMQX + Prometheus + Grafana + cAdvisor) ==="
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

echo "=== [5/5] Setup complete ==="
echo "  C Binaries  : ${BUILD_DIR}/perf_az_mqtt5  ${BUILD_DIR}/perf_paho"
echo "  Rust Binary : ${RUST_PERF_DIR}/target/release/perf_azure_mqtt"
echo "  Results     : ${RESULTS_DIR}/"
echo "  EMQX        : mqtt://localhost:1883  dashboard http://localhost:18083"
echo "  Prometheus  : http://localhost:9090"
echo "  Grafana     : http://localhost:3000  (admin/admin)"
echo ""
echo "Next: run  ./run_az_mqtt5.sh  ./run_paho.sh  ./run_azure_mqtt.sh"
