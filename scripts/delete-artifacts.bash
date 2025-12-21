#!/bin/bash

# Configuration: workflow file name can be provided as the first argument, defaults to llvm.yml
WORKFLOW_FILE_NAME="${1:-llvm.yml}"
RUN_LIMIT=1000

echo "Searching for all runs of workflow ${WORKFLOW_FILE_NAME}..."

# Retrieve all workflow run IDs
RUN_IDS=$(gh run list --workflow "${WORKFLOW_FILE_NAME}" --json databaseId --jq '.[].databaseId' --limit $RUN_LIMIT)

if [ -z "$RUN_IDS" ]; then
    echo "No runs found for workflow ${WORKFLOW_FILE_NAME}."
    exit 0
fi

echo "Found the following Run IDs:"
echo "$RUN_IDS"
echo "---"

# Loop and delete each workflow run (and its Artifacts)
echo "Starting deletion of runs and their Artifacts..."

for RUN_ID in $RUN_IDS; do
    echo "Deleting Run ID: ${RUN_ID}..."
    # Older gh releases may not support --confirm; pipe "y" to avoid interactive prompt.
    if yes | gh run delete "${RUN_ID}"; then
        echo "Successfully deleted Run ${RUN_ID}."
    else
        echo "Failed to delete Run ${RUN_ID}!"
    fi
    sleep 0.5
done

echo ""
echo "---"
echo "All workflow runs and associated Artifacts have been processed."
