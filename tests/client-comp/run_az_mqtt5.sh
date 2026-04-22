#!/usr/bin/env bash
# run_az_mqtt5.sh – Run the az_mqtt5 performance test and capture resource stats.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
RESULTS_DIR="${SCRIPT_DIR}/results"
BINARY="${BUILD_DIR}/perf_az_mqtt5"

HOST="${PERF_HOST:-localhost}"
PORT="${PERF_PORT:-1883}"
MSG_COUNT="${PERF_MSG_COUNT:-10000}"
PAYLOAD="${PERF_PAYLOAD:-128}"
DURATION="${PERF_DURATION:-30}"

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUTFILE="${RESULTS_DIR}/az_mqtt5_${TIMESTAMP}.json"
BROKER_FILE="${RESULTS_DIR}/broker_az_mqtt5_${TIMESTAMP}.json"

mkdir -p "${RESULTS_DIR}"

if [ ! -x "${BINARY}" ]; then
  echo "ERROR: ${BINARY} not found. Run setup.sh first."
  exit 1
fi

echo "=== Running az_mqtt5 perf test ==="
echo "  host=${HOST} port=${PORT} msgs=${MSG_COUNT} payload=${PAYLOAD} duration=${DURATION}s"

# Capture broker stats BEFORE
BROKER_BEFORE=$(curl -s "http://localhost:18083/api/v5/prometheus/stats" 2>/dev/null || echo "")

# Run the test – JSON goes to stdout, progress to stderr
"${BINARY}" "${HOST}" "${PORT}" "${MSG_COUNT}" "${PAYLOAD}" "${DURATION}" > "${OUTFILE}"

# Capture broker stats AFTER
BROKER_AFTER=$(curl -s "http://localhost:18083/api/v5/prometheus/stats" 2>/dev/null || echo "")

# Save broker snapshots
jq -n \
  --arg before "${BROKER_BEFORE}" \
  --arg after "${BROKER_AFTER}" \
  '{"broker_stats_before": $before, "broker_stats_after": $after}' > "${BROKER_FILE}" 2>/dev/null || true

echo "=== az_mqtt5 results ==="
cat "${OUTFILE}"
echo ""
echo "Saved to: ${OUTFILE}"
echo "Broker snapshot: ${BROKER_FILE}"
