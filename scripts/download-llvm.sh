#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <workflow_id>" >&2
  exit 1
fi

WORKFLOW_ID="$1"

mkdir -p artifacts
gh run download "${WORKFLOW_ID}" --dir artifacts

echo "Downloaded artifacts:"
find artifacts -maxdepth 4 -type f -print
