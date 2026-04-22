#!/usr/bin/env bash
# run.sh – One-shot script: setup, run all three clients, compare, teardown.
#
# Usage:
#   ./run.sh                       # defaults
#   PERF_MSG_COUNT=50000 ./run.sh  # override tunables
#   ./run.sh --keep                # skip teardown (leave Docker running)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

KEEP=false
for arg in "$@"; do
  case "${arg}" in
    --keep) KEEP=true ;;
  esac
done

echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║          MQTT Client Comparison – Full Pipeline                ║"
echo "╚══════════════════════════════════════════════════════════════════╝"
echo ""

# ── 1. Setup ──────────────────────────────────────────────────
echo ">>> Step 1/6: Setup (build + infrastructure)"
"${SCRIPT_DIR}/setup.sh"
echo ""

# ── 2. Run az_mqtt5 (C) ──────────────────────────────────────
echo ">>> Step 2/6: Run az_mqtt5 (C)"
"${SCRIPT_DIR}/run_az_mqtt5.sh"
echo ""

# ── 3. Run Paho MQTT C ───────────────────────────────────────
echo ">>> Step 3/6: Run Paho MQTT C"
if [ -x "${SCRIPT_DIR}/build/perf_paho" ]; then
  "${SCRIPT_DIR}/run_paho.sh"
else
  echo "  SKIP: perf_paho not built (libpaho-mqtt-dev not installed)."
fi
echo ""

# ── 4. Run azure_mqtt (Rust) ─────────────────────────────────
echo ">>> Step 4/6: Run azure_mqtt (Rust)"
"${SCRIPT_DIR}/run_azure_mqtt.sh"
echo ""

# ── 5. Compare ───────────────────────────────────────────────
echo ">>> Step 5/6: Compare results"
"${SCRIPT_DIR}/compare.sh"
echo ""

# ── 6. Teardown ──────────────────────────────────────────────
if [ "${KEEP}" = true ]; then
  echo ">>> Step 6/6: Teardown SKIPPED (--keep flag)."
  echo "  Run  ./teardown.sh  when done."
else
  echo ">>> Step 6/6: Teardown"
  "${SCRIPT_DIR}/teardown.sh"
fi

echo ""
echo "Done. Results are in: ${SCRIPT_DIR}/results/"
