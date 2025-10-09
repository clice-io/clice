#!/bin/bash
set -e

# Save original working directory and switch to project root
ORIG_PWD="$(pwd)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "${SCRIPT_DIR}"
cd "${SCRIPT_DIR}/../.."
PROJECT_ROOT="$(pwd)"

trap 'cd "${ORIG_PWD}"' EXIT

# default configurations
COMPILER="clang"
DOCKERFILE_PATH="docker/linux/Dockerfile"

usage() {
cat <<EOF
Usage: $0 [--compiler <gcc|clang>]

Defaults:
  --compiler    ${COMPILER}
EOF
}

# parse command line arguments
while [ "$#" -gt 0 ]; do
    case "$1" in
        --compiler)
            COMPILER="$2"; shift 2;;
        -h|--help)
            usage; exit 0;;
        *)
            echo "Unknown parameter: $1" >&2; usage; exit 1;;
    esac
done

IMAGE_TAG="linux-${COMPILER}"
IMAGE_NAME="clice-io/clice-dev:${IMAGE_TAG}"

echo "==========================================="
echo "Building image: ${IMAGE_NAME}"
echo "Compiler: ${COMPILER}"
echo "Dockerfile: ${DOCKERFILE_PATH}"
echo "==========================================="

# build the docker image with specified arguments
# must run in clice root dir, so that we can mount the project in docker file to acquire essential files
docker buildx build --progress=plain -t "${IMAGE_NAME}" \
    --build-arg COMPILER="${COMPILER}" \
    --build-arg BUILD_SRC="${PROJECT_ROOT}" \
    -f "${DOCKERFILE_PATH}" .

echo "Build complete. Image:${IMAGE_NAME}"
