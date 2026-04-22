#!/usr/bin/env bash
# run_azure_mqtt.sh – Run the azure_mqtt (Rust) performance test and capture resource stats.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RUST_PERF_DIR="${SCRIPT_DIR}/perf_azure_mqtt"
RESULTS_DIR="${SCRIPT_DIR}/results"
BINARY="${RUST_PERF_DIR}/target/release/perf_azure_mqtt"

HOST="${PERF_HOST:-localhost}"
PORT="${PERF_PORT:-1883}"
MSG_COUNT="${PERF_MSG_COUNT:-10000}"
PAYLOAD="${PERF_PAYLOAD:-128}"
DURATION="${PERF_DURATION:-30}"

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUTFILE="${RESULTS_DIR}/azure_mqtt_${TIMESTAMP}.json"
BROKER_FILE="${RESULTS_DIR}/broker_azure_mqtt_${TIMESTAMP}.json"

mkdir -p "${RESULTS_DIR}"

if [ ! -x "${BINARY}" ]; then
  echo "ERROR: ${BINARY} not found. Run setup.sh first."
  exit 1
fi

echo "=== Running azure_mqtt (Rust) perf test ==="
echo "  host=${HOST} port=${PORT} msgs=${MSG_COUNT} payload=${PAYLOAD} duration=${DURATION}s"

# Capture broker stats BEFORE
BROKER_BEFORE=$(curl -s "http://localhost:18083/api/v5/prometheus/stats" 2>/dev/null || echo "")

# Run the test – JSON goes to stdout, progress to stderr
"${BINARY}" "${HOST}" "${PORT}" "${MSG_COUNT}" "${PAYLOAD}" "${DURATION}" > "${OUTFILE}"

# Capture broker stats AFTER
BROKER_AFTER=$(curl -s "http://localhost:18083/api/v5/prometheus/stats" 2>/dev/null || echo "")

jq -n \
  --arg before "${BROKER_BEFORE}" \
  --arg after "${BROKER_AFTER}" \
  '{"broker_stats_before": $before, "broker_stats_after": $after}' > "${BROKER_FILE}" 2>/dev/null || true

echo "=== azure_mqtt results ==="
cat "${OUTFILE}"
echo ""
echo "Saved to: ${OUTFILE}"
echo "Broker snapshot: ${BROKER_FILE}"
