#!/usr/bin/env bash
# teardown.sh – Stop and remove all perf-test Docker containers.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
docker compose -f "${SCRIPT_DIR}/docker-compose.yml" down -v
echo "Infrastructure removed.  Result files in results/ are preserved."
