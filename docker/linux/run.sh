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
RESET="false"

usage() {
cat <<EOF
Usage: $0 [--compiler <gcc|clang>] [--reset]

Defaults:
  --compiler    ${COMPILER}
  --reset       (re-create the container)
EOF
}

# parse command line arguments
while [ "$#" -gt 0 ]; do
  case "$1" in
    --compiler)
      COMPILER="$2"; shift 2;;
    --reset)
      RESET="true"; shift 1;;
    -h|--help)
      usage; exit 0;;
    *) echo "Unknown parameter: $1"; usage; exit 1;;
  esac
done

IMAGE_TAG="linux-${COMPILER}"
IMAGE_NAME="clice-io/clice-dev:${IMAGE_TAG}"
CONTAINER_NAME="clice-dev-linux-${COMPILER}"

# If the image doesn't exist, build it automatically by invoking build.sh
if ! docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
  echo "Image ${IMAGE_NAME} not found, invoking build.sh to create it..."
  ./docker/linux/build.sh --compiler "${COMPILER}"
fi

# Handle --reset: remove the existing container if it exists
if [ "${RESET}" = "true" ]; then
  if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "Resetting container: stopping and removing existing container ${CONTAINER_NAME}..."
    docker stop "${CONTAINER_NAME}" >/dev/null 2>&1 || true
    docker rm "${CONTAINER_NAME}" >/dev/null 2>&1
    echo "Container ${CONTAINER_NAME} has been removed."
  else
    echo "Container ${CONTAINER_NAME} does not exist, no need to reset."
  fi
  exit 0
fi

CONTAINER_WORKDIR="/clice"

# Check if the container exists
if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
  echo "==========================================="
  echo "Attaching to existing container: ${CONTAINER_NAME}"
  echo "From image: ${IMAGE_NAME}"
  echo "Project mount: ${PROJECT_ROOT} -> ${CONTAINER_WORKDIR}"
  echo "==========================================="
  docker start "${CONTAINER_NAME}" >/dev/null
  docker exec -it -w "${CONTAINER_WORKDIR}" "${CONTAINER_NAME}" /bin/bash
  exit 0
fi

DOCKER_RUN_ARGS=(-it -w "${CONTAINER_WORKDIR}")
DOCKER_RUN_ARGS+=(--name "${CONTAINER_NAME}")
DOCKER_RUN_ARGS+=(--mount "type=bind,src=${PROJECT_ROOT},target=${CONTAINER_WORKDIR}")

echo "==========================================="
echo "Creating and running new container: ${CONTAINER_NAME}"
echo "From image: ${IMAGE_NAME}"
echo "Project mount: ${PROJECT_ROOT} -> ${CONTAINER_WORKDIR}"
echo "==========================================="

docker run "${DOCKER_RUN_ARGS[@]}" "${IMAGE_NAME}"
