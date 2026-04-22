#!/usr/bin/env bash
# compare.sh – Find the latest result files for each client, merge, and display
# a side-by-side comparison table.  Outputs both a human-readable table to
# the terminal and a consolidated JSON to results/comparison_<ts>.json.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/results"

# Find latest result file per client
AZ_FILE="$(ls -t "${RESULTS_DIR}"/az_mqtt5_*.json 2>/dev/null | head -1 || true)"
PAHO_FILE="$(ls -t "${RESULTS_DIR}"/paho_*.json 2>/dev/null | head -1 || true)"

if [ -z "${AZ_FILE}" ] && [ -z "${PAHO_FILE}" ]; then
  echo "ERROR: No result files found in ${RESULTS_DIR}/."
  echo "Run  ./run_az_mqtt5.sh  and  ./run_paho.sh  first."
  exit 1
fi

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
COMP_FILE="${RESULTS_DIR}/comparison_${TIMESTAMP}.json"

# Build consolidated JSON
jq -n \
  --slurpfile az <([ -n "${AZ_FILE}" ] && cat "${AZ_FILE}" || echo "null") \
  --slurpfile paho <([ -n "${PAHO_FILE}" ] && cat "${PAHO_FILE}" || echo "null") \
  '{
    timestamp: "'"${TIMESTAMP}"'",
    az_mqtt5: $az[0],
    paho_mqtt_c: $paho[0]
  }' > "${COMP_FILE}"

echo "Consolidated JSON saved to: ${COMP_FILE}"
echo ""

# ---- Human-readable table ----

header_fmt="%-28s %18s %18s %12s\n"
row_fmt="%-28s %18s %18s %12s\n"

printf "\n"
printf "╔══════════════════════════════════════════════════════════════════════════════╗\n"
printf "║                     MQTT CLIENT PERFORMANCE COMPARISON                     ║\n"
printf "╚══════════════════════════════════════════════════════════════════════════════╝\n"
printf "\n"
printf "${header_fmt}" "Metric" "az_mqtt5" "paho_mqtt_c" "Delta"
printf "%-28s %18s %18s %12s\n" "----------------------------" "------------------" "------------------" "------------"

# Helper: extract a numeric field from a JSON file, default to "-"
val() {
  local file=$1 field=$2
  if [ -n "${file}" ] && [ -f "${file}" ]; then
    jq -r ".${field} // \"-\"" "${file}" 2>/dev/null || echo "-"
  else
    echo "-"
  fi
}

# Helper: compute percentage delta  (az vs paho).  Positive = az is higher.
delta() {
  local a=$1 b=$2
  if [ "${a}" = "-" ] || [ "${b}" = "-" ] || [ "${b}" = "0" ]; then
    echo "-"
    return
  fi
  awk "BEGIN { d = (${a} - ${b}) / ${b} * 100; printf \"%+.1f%%\", d }"
}

print_row() {
  local label=$1 field=$2 fmt=${3:-%.1f}
  local a=$(val "${AZ_FILE}" "${field}")
  local b=$(val "${PAHO_FILE}" "${field}")
  local d=$(delta "${a}" "${b}")
  printf "${row_fmt}" "${label}" "${a}" "${b}" "${d}"
}

print_row "Messages sent"           "messages_sent"        "%d"
print_row "Messages received"       "messages_received"    "%d"
print_row "PUBACKs received"        "pubacks_received"     "%d"
print_row "Elapsed (sec)"           "elapsed_sec"          "%.3f"
print_row "Send rate (msg/s)"       "send_rate_msg_sec"    "%.1f"
print_row "Recv rate (msg/s)"       "recv_rate_msg_sec"    "%.1f"
print_row "User CPU (sec)"          "user_cpu_sec"         "%.3f"
print_row "System CPU (sec)"        "sys_cpu_sec"          "%.3f"
print_row "Total CPU (sec)"         "total_cpu_sec"        "%.3f"
print_row "Peak RSS (bytes)"        "peak_rss_bytes"       "%d"

printf "\n"
echo "(Delta = (az_mqtt5 − paho) / paho × 100.  Negative = az_mqtt5 uses fewer resources.)"
echo ""

# Broker stats diff (if available)
AZ_BROKER="$(ls -t "${RESULTS_DIR}"/broker_az_mqtt5_*.json 2>/dev/null | head -1 || true)"
PAHO_BROKER="$(ls -t "${RESULTS_DIR}"/broker_paho_*.json 2>/dev/null | head -1 || true)"

if [ -n "${AZ_BROKER}" ] || [ -n "${PAHO_BROKER}" ]; then
  echo "Broker stat snapshots:"
  [ -n "${AZ_BROKER}" ]  && echo "  az_mqtt5 run : ${AZ_BROKER}"
  [ -n "${PAHO_BROKER}" ] && echo "  paho run     : ${PAHO_BROKER}"
  echo "(Inspect these files to compare broker-side resource usage between runs.)"
fi
