#!/usr/bin/env bash
# compare.sh – Find the latest result files for each client, merge, and display
# a side-by-side comparison table including memory footprint data.
# Outputs both a human-readable table and a consolidated JSON.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/results"
BUILD_DIR="${SCRIPT_DIR}/build"
RUST_PERF_DIR="${SCRIPT_DIR}/perf_azure_mqtt"

# Find latest result file per client
AZ_FILE="$(ls -t "${RESULTS_DIR}"/az_mqtt5_*.json 2>/dev/null | head -1 || true)"
PAHO_FILE="$(ls -t "${RESULTS_DIR}"/paho_*.json 2>/dev/null | head -1 || true)"
RUST_FILE="$(ls -t "${RESULTS_DIR}"/azure_mqtt_*.json 2>/dev/null | head -1 || true)"

if [ -z "${AZ_FILE}" ] && [ -z "${PAHO_FILE}" ] && [ -z "${RUST_FILE}" ]; then
  echo "ERROR: No result files found in ${RESULTS_DIR}/."
  echo "Run  ./run.sh  first."
  exit 1
fi

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
COMP_FILE="${RESULTS_DIR}/comparison_${TIMESTAMP}.json"

# ── Collect binary sizes ──────────────────────────────────────
bin_size() {
  local f=$1
  if [ -f "${f}" ]; then
    stat --format='%s' "${f}" 2>/dev/null || stat -f '%z' "${f}" 2>/dev/null || echo "0"
  else
    echo "-"
  fi
}

text_size() {
  local f=$1
  if [ -f "${f}" ] && command -v size &>/dev/null; then
    size "${f}" 2>/dev/null | awk 'NR==2{print $1}' || echo "-"
  else
    echo "-"
  fi
}

AZ_BIN="${BUILD_DIR}/perf_az_mqtt5"
PAHO_BIN="${BUILD_DIR}/perf_paho"
RUST_BIN="${RUST_PERF_DIR}/target/release/perf_azure_mqtt"

AZ_BIN_SIZE=$(bin_size "${AZ_BIN}")
PAHO_BIN_SIZE=$(bin_size "${PAHO_BIN}")
RUST_BIN_SIZE=$(bin_size "${RUST_BIN}")

AZ_TEXT=$(text_size "${AZ_BIN}")
PAHO_TEXT=$(text_size "${PAHO_BIN}")
RUST_TEXT=$(text_size "${RUST_BIN}")

# Build consolidated JSON
jq -n \
  --slurpfile az <([ -n "${AZ_FILE}" ] && cat "${AZ_FILE}" || echo "null") \
  --slurpfile paho <([ -n "${PAHO_FILE}" ] && cat "${PAHO_FILE}" || echo "null") \
  --slurpfile rust <([ -n "${RUST_FILE}" ] && cat "${RUST_FILE}" || echo "null") \
  --arg az_bin_size "${AZ_BIN_SIZE}" \
  --arg paho_bin_size "${PAHO_BIN_SIZE}" \
  --arg rust_bin_size "${RUST_BIN_SIZE}" \
  --arg az_text "${AZ_TEXT}" \
  --arg paho_text "${PAHO_TEXT}" \
  --arg rust_text "${RUST_TEXT}" \
  '{
    timestamp: "'"${TIMESTAMP}"'",
    az_mqtt5: ($az[0] + {binary_size: ($az_bin_size | tonumber? // null), text_section: ($az_text | tonumber? // null)}),
    paho_mqtt_c: ($paho[0] + {binary_size: ($paho_bin_size | tonumber? // null), text_section: ($paho_text | tonumber? // null)}),
    azure_mqtt_rust: ($rust[0] + {binary_size: ($rust_bin_size | tonumber? // null), text_section: ($rust_text | tonumber? // null)})
  }' > "${COMP_FILE}"

echo "Consolidated JSON saved to: ${COMP_FILE}"
echo ""

# ── Human-readable tables ─────────────────────────────────────

header_fmt="%-24s %16s %16s %16s\n"
row_fmt="%-24s %16s %16s %16s\n"

printf "\n"
printf "╔══════════════════════════════════════════════════════════════════════════════════╗\n"
printf "║                      MQTT CLIENT PERFORMANCE COMPARISON                        ║\n"
printf "╚══════════════════════════════════════════════════════════════════════════════════╝\n"
printf "\n"
printf "${header_fmt}" "Metric" "az_mqtt5 (C)" "paho_mqtt (C)" "azure_mqtt (Rust)"
printf "%-24s %16s %16s %16s\n" "------------------------" "----------------" "----------------" "----------------"

val() {
  local file=$1 field=$2
  if [ -n "${file}" ] && [ -f "${file}" ]; then
    jq -r ".${field} // \"-\"" "${file}" 2>/dev/null || echo "-"
  else
    echo "-"
  fi
}

print_row() {
  local label=$1 field=$2
  local a=$(val "${AZ_FILE}" "${field}")
  local b=$(val "${PAHO_FILE}" "${field}")
  local c=$(val "${RUST_FILE}" "${field}")
  printf "${row_fmt}" "${label}" "${a}" "${b}" "${c}"
}

print_row "Messages sent"        "messages_sent"
print_row "Messages received"    "messages_received"
print_row "PUBACKs received"     "pubacks_received"
print_row "Elapsed (sec)"        "elapsed_sec"
print_row "Send rate (msg/s)"    "send_rate_msg_sec"
print_row "Recv rate (msg/s)"    "recv_rate_msg_sec"
print_row "User CPU (sec)"       "user_cpu_sec"
print_row "System CPU (sec)"     "sys_cpu_sec"
print_row "Total CPU (sec)"      "total_cpu_sec"
print_row "Peak RSS (bytes)"     "peak_rss_bytes"

# ── Memory Footprint Table ────────────────────────────────────

fmt_human() {
  local v=$1
  if [ "${v}" = "-" ] || [ -z "${v}" ]; then echo "-"; return; fi
  if [ "${v}" -ge 1048576 ] 2>/dev/null; then
    awk "BEGIN { printf \"%.1f MB\", ${v}/1048576 }"
  elif [ "${v}" -ge 1024 ] 2>/dev/null; then
    awk "BEGIN { printf \"%.1f KB\", ${v}/1024 }"
  else
    echo "${v} B"
  fi
}

printf "\n"
printf "%-24s %16s %16s %16s\n" "── Memory Footprint ──" "" "" ""
printf "%-24s %16s %16s %16s\n" "------------------------" "----------------" "----------------" "----------------"
printf "${row_fmt}" "Binary on disk"  "$(fmt_human "${AZ_BIN_SIZE}")" "$(fmt_human "${PAHO_BIN_SIZE}")" "$(fmt_human "${RUST_BIN_SIZE}")"
printf "${row_fmt}" ".text section"   "$(fmt_human "${AZ_TEXT}")"     "$(fmt_human "${PAHO_TEXT}")"     "$(fmt_human "${RUST_TEXT}")"

printf "\n"

# ── Broker snapshots ──────────────────────────────────────────

AZ_BROKER="$(ls -t "${RESULTS_DIR}"/broker_az_mqtt5_*.json 2>/dev/null | head -1 || true)"
PAHO_BROKER="$(ls -t "${RESULTS_DIR}"/broker_paho_*.json 2>/dev/null | head -1 || true)"
RUST_BROKER="$(ls -t "${RESULTS_DIR}"/broker_azure_mqtt_*.json 2>/dev/null | head -1 || true)"

if [ -n "${AZ_BROKER}" ] || [ -n "${PAHO_BROKER}" ] || [ -n "${RUST_BROKER}" ]; then
  echo "Broker stat snapshots:"
  [ -n "${AZ_BROKER}" ]   && echo "  az_mqtt5 run    : ${AZ_BROKER}"
  [ -n "${PAHO_BROKER}" ]  && echo "  paho run        : ${PAHO_BROKER}"
  [ -n "${RUST_BROKER}" ]  && echo "  azure_mqtt run  : ${RUST_BROKER}"
  echo "(Inspect these files to compare broker-side resource usage between runs.)"
fi
