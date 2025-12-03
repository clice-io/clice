#!/bin/bash
# ========================================================================
# Clice Development Container Runner
# ========================================================================

set -e

# ========================================================================
# 🔧 Environment Setup
# ========================================================================

# Source common utilities
source "$(dirname "${BASH_SOURCE[0]}")/utility/common.sh"

# Save original working directory and switch to project root
ORIG_PWD="$(pwd)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}/../.."
PROJECT_ROOT="$(pwd)"

trap 'cd "${ORIG_PWD}"' EXIT

COMPILER="${DEFAULT_COMPILER}"
RESET="false"
UPDATE="false"
VERSION="${DEFAULT_VERSION}"
COMMAND=""
CONTAINER_WORKDIR="${DEFAULT_CONTAINER_WORKDIR}"

# ========================================================================
# 📚 Usage Information
# ========================================================================

usage() {
cat <<EOF
🚀 Clice Development Container Runner

Usage: $0 [OPTIONS] [COMMAND]

OPTIONS:
  --compiler <gcc|clang>     Target compiler (default: ${COMPILER})
  --reset                    Remove existing container
  --update                   Pull latest image and update
  --version <version>        Use specific version (default: ${VERSION})
  --help, -h                 Show this help message

EXAMPLES:
  $0                         Run container (build if not exists)
  $0 --compiler gcc          Run container with GCC compiler
  $0 --reset                 Remove container and recreate
  $0 --update                Pull latest image and update
  $0 bash                    Run specific command in container
EOF
}

# ========================================================================
# 🔍 Command Line Parsing
# ========================================================================

while [ "$#" -gt 0 ]; do
  case "$1" in
    --compiler)
      COMPILER="$2"; shift 2;;
    --reset)
      RESET="true"; shift 1;;
    --update)
      UPDATE="true"; shift 1;;
    --version)
      VERSION="$2"; shift 2;;
    -h|--help)
      usage; exit 0;;
    --)
      shift; COMMAND="$*"; break;;
    -*)
      echo "❌ Unknown parameter: $1" >&2; usage; exit 1;;
    *)
      COMMAND="$*"; break;;
  esac
done

# ========================================================================
# 🏷️ Container and Image Naming
# ========================================================================

PACKED_IMAGE_NAME=$(get_packed_image_name "${COMPILER}" "${VERSION}")
EXPANDED_IMAGE_NAME=$(get_expanded_image_name "${COMPILER}" "${VERSION}")
CONTAINER_NAME=$(get_container_name "${COMPILER}" "${VERSION}")

# ========================================================================
# 🚀 Main Execution
# ========================================================================

echo "========================================================================="
echo "🚀 CLICE DEVELOPMENT CONTAINER RUNNER"
echo "========================================================================="
echo "🏷️ Image: ${EXPANDED_IMAGE_NAME}"
echo "🏷️ Version: ${VERSION}"
echo "🐳 Container: ${CONTAINER_NAME}"
echo "🔧 Compiler: ${COMPILER}"
echo "📁 Project Root: ${PROJECT_ROOT}"
echo "========================================================================="

# ========================================================================
# 🐳 Container Management
# ========================================================================

# Handle --reset: remove the existing container if it exists
if [ "${RESET}" = "true" ]; then
  if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "🔄 Removing existing container: ${CONTAINER_NAME}..."
    docker stop "${CONTAINER_NAME}" >/dev/null 2>&1 || true
    docker rm "${CONTAINER_NAME}" >/dev/null 2>&1
    echo "✅ Container ${CONTAINER_NAME} has been removed."
  else
    echo "ℹ️ Container ${CONTAINER_NAME} does not exist."
  fi
  echo "🏁 Reset completed. Run again without --reset to create new container."
  exit 0
fi

# ========================================================================
# 🏗️ Image Management
# ========================================================================

# Check if we need to update/pull the packed image
UPDATE_REASON=""
if [ "$UPDATE" = "true" ]; then
  UPDATE_REASON="🔄 Force updating image..."
elif ! docker image inspect "${PACKED_IMAGE_NAME}" >/dev/null 2>&1; then
  UPDATE_REASON="🔄 Packed image ${PACKED_IMAGE_NAME} not found locally, pulling..."
fi

if [ -n "$UPDATE_REASON" ]; then

  echo "${UPDATE_REASON}"

  # Remove existing expanded image before pulling (avoid conflicts)
  if docker image inspect "${EXPANDED_IMAGE_NAME}" >/dev/null 2>&1; then
    echo "🧹 Cleaning existing expanded image: ${EXPANDED_IMAGE_NAME}..."
    if ! docker rmi "${EXPANDED_IMAGE_NAME}" >/dev/null 2>&1; then
      echo "❌ Failed to remove expanded image: ${EXPANDED_IMAGE_NAME}"
      echo "💡 This usually means a container is still using this image."
      echo "🔧 Please run: $0 --reset to remove the container first, then try --update again."
      echo "⚠️ This ensures your container data safety - we won't accidentally delete your container."
      exit 1
    fi
    echo "✅ Expanded image removed successfully"
  fi
  
  echo "📥 Pulling ${PACKED_IMAGE_NAME} from registry..."
  if docker pull "${PACKED_IMAGE_NAME}"; then
    echo "✅ Successfully pulled image: ${PACKED_IMAGE_NAME}"
  else
    echo "❌ Could not pull image: ${PACKED_IMAGE_NAME}"
    echo "💡 Please check if the image exists in the registry"
    exit 1
  fi

  echo "🏁 Update completed."
fi

# ========================================================================
# 🏗️ Auto-Expand Packed Image to Development Image
# ========================================================================

# At this point, packed image is guaranteed to exist (either pulled or already present)
# Check if expanded development image exists, if not, expand it from packed image
if ! docker image inspect "${EXPANDED_IMAGE_NAME}" >/dev/null 2>&1; then
  echo "========================================================================="
  echo "🏗️ EXPANDING PACKED IMAGE TO DEVELOPMENT IMAGE"
  echo "========================================================================="
  echo "📦 Source (Packed): ${PACKED_IMAGE_NAME}"
  echo "🎯 Target (Expanded): ${EXPANDED_IMAGE_NAME}"
  echo "========================================================================="
  
  # Run packed image container and execute its internal build.sh for expansion
  # To keep the expansion process consistent and reliable, we use the build.sh script from the container itself.
  # 
  # Since newer Docker versions don't include CLI without installation, we can't use docker buildx directly.
  # Instead, we use chroot approach:
  # 1. Mount host root directory to a temp folder inside container
  # 2. Copy /clice to host temp directory
  # 3. chroot into host root and execute build.sh
  #
  # Mounts:
  # • / (host root) - Mount to temp directory for chroot access
  
  # Create temp directory on host for chroot
  HOST_TEMP_DIR=$(mktemp -d -p /tmp clice-expand.XXXXXX)
  
  echo "📁 Created host temp directory: ${HOST_TEMP_DIR}"
  echo "🔄 Preparing chroot environment..."
  
  if docker run --rm \
      -v "/:/host-root" \
      -e "HOST_TEMP_DIR=${HOST_TEMP_DIR}" \
      -e "COMPILER=${COMPILER}" \
      -e "VERSION=${VERSION}" \
      "${PACKED_IMAGE_NAME}" \
      /bin/bash -c '
        set -e
        echo "📦 Copying /clice to host temp directory..."
        cp -r /clice "/host-root${HOST_TEMP_DIR}/"
        
        echo "🔧 Executing build.sh via chroot..."
        chroot /host-root /bin/bash -c "
          cd ${HOST_TEMP_DIR}/clice && \
          ./docker/linux/build.sh --stage expanded-image --compiler ${COMPILER} --version ${VERSION} --debug
        "
        
        echo "🧹 Cleaning up temp directory..."
        rm -rf "/host-root${HOST_TEMP_DIR}"
      '; then
    # Clean up host temp directory (in case container cleanup failed)
    rm -rf "${HOST_TEMP_DIR}" 2>/dev/null || true
    echo "========================================================================="
    echo "✅ EXPANSION COMPLETED SUCCESSFULLY"
    echo "========================================================================="
    echo "🎉 Development image created: ${EXPANDED_IMAGE_NAME}"
    echo "📦 Ready for container creation"
    echo "========================================================================="
  else
    echo "========================================================================="
    echo "❌ EXPANSION FAILED"
    echo "========================================================================="
    exit 1
  fi
else
  echo "✅ Development image already exists: ${EXPANDED_IMAGE_NAME}"
fi

# Check if the container exists and is using the current development image
if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
  echo "🔍 Found existing container: ${CONTAINER_NAME}"
  
  # Check if container uses current development image (compare image IDs)
  CONTAINER_IMAGE_ID=$(docker inspect --format='{{.Image}}' "${CONTAINER_NAME}" 2>/dev/null || echo "")
  EXPECTED_IMAGE_ID=$(docker inspect --format='{{.Id}}' "${EXPANDED_IMAGE_NAME}" 2>/dev/null || echo "")
  
  # Check target image and container match
  if [ -n "$CONTAINER_IMAGE_ID" ] && [ -n "$EXPECTED_IMAGE_ID" ] && [ "$CONTAINER_IMAGE_ID" = "$EXPECTED_IMAGE_ID" ]; then
    echo "✅ Container is using current development image"
    echo "🚀 Starting and attaching to container..."
  else
    CONTAINER_IMAGE_NAME=$(docker inspect --format='{{.Config.Image}}' "${CONTAINER_NAME}" 2>/dev/null || echo "unknown")
    echo "⚠️ WARNING: Container image mismatch!"
    echo "   📦 Container using: ${CONTAINER_IMAGE_NAME} (ID: ${CONTAINER_IMAGE_ID})"
    echo "   🎯 Expected: ${EXPANDED_IMAGE_NAME} (ID: ${EXPECTED_IMAGE_ID})"
    echo ""
    echo "💡 Your container is using a different image version."
    echo "🛡️ To ensure data safety, please:"
    echo "   1. Save any important work from the current container"
    echo "   2. Run: $0 --reset to remove the outdated container"
    echo "   3. Run: $0 to create a new container with the latest image"
    echo ""
    echo "🚀 For now, connecting to your existing container..."
  fi
    
  docker start "${CONTAINER_NAME}" >/dev/null
    
  if [ -n "$COMMAND" ]; then
    docker exec -it -w "${CONTAINER_WORKDIR}" "${CONTAINER_NAME}" bash -c "$COMMAND"
  else
    docker exec -it -w "${CONTAINER_WORKDIR}" "${CONTAINER_NAME}" /bin/bash
  fi
  exit 0
fi

# Create new container
DOCKER_RUN_ARGS=(-it -w "${CONTAINER_WORKDIR}")
DOCKER_RUN_ARGS+=(--name "${CONTAINER_NAME}")
DOCKER_RUN_ARGS+=(--mount "type=bind,src=${PROJECT_ROOT},target=${CONTAINER_WORKDIR}")

echo "========================================================================="
echo "🚀 Creating new container: ${CONTAINER_NAME}"
echo "📦 From image: ${EXPANDED_IMAGE_NAME}"
echo "📁 Project mount: ${PROJECT_ROOT} -> ${CONTAINER_WORKDIR}"
echo "========================================================================="

if [ -n "$COMMAND" ]; then
  echo "🏃 Executing command: $COMMAND"
  docker run --rm "${DOCKER_RUN_ARGS[@]}" "${EXPANDED_IMAGE_NAME}" bash -c "$COMMAND"
else
  echo "🐚 Starting interactive shell..."
  docker run "${DOCKER_RUN_ARGS[@]}" "${EXPANDED_IMAGE_NAME}"
fi