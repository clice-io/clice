#!/usr/bin/env bash

# Periodically logs disk and memory usage to a file.
# Usage: monitor-resources.sh [log-file]

set -uo pipefail

LOG_FILE="${1:-ci-resource-usage.log}"
INTERVAL="${CLICE_MONITOR_INTERVAL:-30}"
OS="$(uname -s)"

echo "Resource monitor started for ${OS}, interval ${INTERVAL}s, writing to ${LOG_FILE}" >> "${LOG_FILE}"

log_disk() {
  echo "[$(date -u +"%Y-%m-%dT%H:%M:%SZ")] ===== Disk =====" >> "${LOG_FILE}"
  df -h >> "${LOG_FILE}" 2>&1
}

log_mem() {
  echo "[$(date -u +"%Y-%m-%dT%H:%M:%SZ")] ===== Memory =====" >> "${LOG_FILE}"
  case "${OS}" in
    Linux)
      if command -v free >/dev/null 2>&1; then
        free -h >> "${LOG_FILE}" 2>&1
      else
        cat /proc/meminfo >> "${LOG_FILE}" 2>&1
      fi
      ;;
    Darwin)
      if command -v vm_stat >/dev/null 2>&1; then
        PAGESIZE="$(sysctl -n hw.pagesize 2>/dev/null || echo 4096)"
        TOTAL_MEM="$(sysctl -n hw.memsize 2>/dev/null || echo 0)"
        echo "Total Memory: ${TOTAL_MEM} bytes" >> "${LOG_FILE}"
        vm_stat >> "${LOG_FILE}" 2>&1
        # Derive an approximate free/used breakdown to make the log easier to read.
        FREE_PAGES="$(vm_stat | awk '/(Pages free|Pages speculative)/ {gsub(/\\./,\"\",$3); total+=$3} END {print total+0}')"
        if [[ -n "${FREE_PAGES}" ]]; then
          FREE_BYTES=$((FREE_PAGES * PAGESIZE))
          echo "Approx Free: ${FREE_BYTES} bytes" >> "${LOG_FILE}"
        fi
      else
        echo "vm_stat unavailable" >> "${LOG_FILE}"
      fi
      ;;
    *)
      echo "Unsupported OS for memory logging: ${OS}" >> "${LOG_FILE}"
      ;;
  esac
}

while true; do
  log_disk
  log_mem
  echo "" >> "${LOG_FILE}"
  sleep "${INTERVAL}"
done
