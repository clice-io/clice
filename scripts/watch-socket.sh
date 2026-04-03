#!/usr/bin/env bash

set -euo pipefail

BUILD_CMD=${BUILD_CMD:-".clice/build.sh"}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

if ! command -v watchexec >/dev/null 2>&1; then
  echo "watchexec is not installed or not in PATH." >&2
  exit 1
fi

exec watchexec \
  --project-origin "${ROOT_DIR}" \
  --watch "${ROOT_DIR}/config" \
  --watch "${ROOT_DIR}/src" \
  --watch "${ROOT_DIR}/cmake" \
  --watch "${ROOT_DIR}/CMakeLists.txt" \
  --restart \
  --clear \
  --shell=bash \
  -- "$BUILD_CMD && ./build/bin/clice --mode socket --port 50051"
