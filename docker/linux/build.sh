#!/bin/bash
# ========================================================================
# Clice Development Container Builder
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
BUILD_STAGE="${DEFAULT_BUILD_STAGE}"
CACHE_FROM=""
CACHE_TO=""
VERSION="${DEFAULT_VERSION}"
REBUILD="false"
DEBUG="false"

# ========================================================================
# 📚 Usage Information
# ========================================================================

usage() {
cat <<EOF
🚀 Clice Development Container Builder

Usage: $0 [OPTIONS]

OPTIONS:
  --compiler <gcc|clang>     Target compiler (default: ${COMPILER})
  --cache-from <image>       Use cache from specified image
  --cache-to <image>         Push cache to specified image and log cache operations
  --version <version>        Set version tag (default: ${VERSION})
  --stage <stage>            Build specific stage (packed-image or expanded-image)
  --rebuild                  Force rebuild even if image exists
  --debug                    Enable interactive debug mode (requires Docker 23.0+)
  --help, -h                 Show this help message

EXAMPLES:
  $0                           Build development container with clang
  $0 --compiler gcc            Build development container with gcc  
  $0 --stage packed-image      Build only the release image
  $0 --stage expanded-image    Expand release image to development image
  $0 --version v1.0.0          Build versioned container (v1.0.0)
  $0 --rebuild                 Force rebuild existing image
  $0 --debug                   Build with interactive debug mode
  $0 --cache-from clice-io/clice-dev:cache  Use cache from existing image
  $0 --cache-to type=registry,ref=myregistry/myimage:cache   Push cache

DEBUG MODE:
  --debug enables interactive debugging with docker buildx debug build
  Requires Docker 23.0+ with BuildKit experimental features
  On build failure, you can use debug commands to inspect the build state
EOF
}

# ========================================================================
# 🔍 Command Line Parsing
# ========================================================================

while [ "$#" -gt 0 ]; do
    case "$1" in
        --compiler)
            COMPILER="$2"; shift 2;;
        --cache-from)
            CACHE_FROM="$2"; shift 2;;
        --cache-to)
            CACHE_TO="$2"; shift 2;;
        --version)
            VERSION="$2"; shift 2;;
        --stage)
            BUILD_STAGE="$2"; shift 2;;
        --rebuild)
            REBUILD="true"; shift 1;;
        --debug)
            DEBUG="true"; shift 1;;
        -h|--help)
            usage; exit 0;;
        *)
            echo "❌ Unknown parameter: $1" >&2; usage; exit 1;;
    esac
done

# ========================================================================
# 🏷️ Image Naming
# ========================================================================

IMAGE_TAG=$(get_image_tag "${COMPILER}" "${VERSION}")
PACKED_IMAGE_NAME=$(get_packed_image_name "${COMPILER}" "${VERSION}")
EXPANDED_IMAGE_NAME=$(get_expanded_image_name "${COMPILER}" "${VERSION}")

# Set the target image name based on build stage
if [ "$BUILD_STAGE" = "packed-image" ]; then
    TARGET_IMAGE_NAME="$PACKED_IMAGE_NAME"
elif [ "$BUILD_STAGE" = "expanded-image" ]; then
    TARGET_IMAGE_NAME="$EXPANDED_IMAGE_NAME"
else
    TARGET_IMAGE_NAME="clice-dev_container-debug_build-$BUILD_STAGE"
    echo "🔧 Debug Building Intermediate Stage: $BUILD_STAGE" >&2; usage;
fi

# ========================================================================
# 🚀 Build Process
# ========================================================================

echo "========================================================================="
echo "🚀 CLICE DEVELOPMENT CONTAINER BUILDER"
echo "========================================================================="
echo "📦 Image: ${TARGET_IMAGE_NAME}"
echo "🏷️ Version: ${VERSION}"
echo "🔧 Compiler: ${COMPILER}"
echo "🐳 Dockerfile: ${DOCKERFILE_PATH}"
echo "📁 Project Root: ${PROJECT_ROOT}"
echo "⚡ Parallel Build: Enabled"
if [ -n "$CACHE_FROM" ]; then
    echo "💾 Cache From: ${CACHE_FROM}"
fi
if [ -n "$CACHE_TO" ]; then
    echo "💾 Cache To: ${CACHE_TO}"
fi
echo "========================================================================="

# ========================================================================
# 🔄 Auto-Expansion Logic (Release → Development)
# ========================================================================

# Build the target image
echo "🔍 Checking for target image: ${TARGET_IMAGE_NAME}"

# Handle REBUILD flag - clean up existing images
if [ "$REBUILD" = "true" ]; then
    echo "🔄 Force rebuild requested - cleaning up existing images..."
    
    # Clean up target image
    if docker image inspect "${TARGET_IMAGE_NAME}" >/dev/null 2>&1; then
        echo "🧹 Removing existing target image: ${TARGET_IMAGE_NAME}"
        docker rmi "${TARGET_IMAGE_NAME}" || true
    fi   
fi

BUILD_ARGS=(
    "--progress=plain"
    "--target"
    "${BUILD_STAGE}"
    "--build-arg"
    "COMPILER=${COMPILER}"
    "--build-arg"
    "VERSION=${VERSION}"
    "--build-arg"
    "PACKED_IMAGE_NAME=${PACKED_IMAGE_NAME}"
    "--build-arg"
    "CLICE_DIR=${CLICE_DIR}"
    "--build-arg"
    "BUILDKIT_INLINE_CACHE=1"
)

if [ -n "$CACHE_FROM" ]; then
    BUILD_ARGS+=("--cache-from=${CACHE_FROM}")
fi

if [ -n "$CACHE_TO" ]; then
    BUILD_ARGS+=("--cache-to=${CACHE_TO}")
    echo "📝 Starting build with cache-to logging enabled..."
fi

BUILD_ARGS+=("-t" "${TARGET_IMAGE_NAME}" "-f" "${DOCKERFILE_PATH}" ".")

# Execute with or without debug mode
if [ "$DEBUG" = "true" ]; then
    # Enable BuildKit experimental features for debug mode
    echo "🐛 Debug mode enabled (BUILDX_EXPERIMENTAL=1)"

    export BUILDX_EXPERIMENTAL=1
    BUILD_COMMAND="docker buildx debug --invoke /bin/bash build"
else
    BUILD_COMMAND="docker buildx build"
fi

echo "🔨 Build command: ${BUILD_COMMAND} ${BUILD_ARGS[*]}"
${BUILD_COMMAND} "${BUILD_ARGS[@]}"

BUILD_SUCCESS=$?

if [ -n "$CACHE_TO" ]; then
    echo "💾 Cache operations completed. Cache pushed to: ${CACHE_TO}"
fi

# ========================================================================
# 📊 Post-Build Information
# ========================================================================

BUILD_SUCCESS=$?

if [ $BUILD_SUCCESS -eq 0 ]; then
    echo "========================================================================="
    echo "✅ BUILD COMPLETED SUCCESSFULLY!"
    echo "========================================================================="
    echo "📦 Image Name: ${TARGET_IMAGE_NAME}"
    echo "🏷️ Image Tag: ${IMAGE_TAG}"
    echo "🔧 Compiler: ${COMPILER}"
    echo "🏗️ Build Stage: ${BUILD_STAGE}"
    
    # Get image information
    if command -v docker &> /dev/null; then
        echo ""
        echo "📊 IMAGE INFORMATION:"
        docker image inspect "${TARGET_IMAGE_NAME}" --format="Size: {{.Size}} bytes ({{.VirtualSize}} virtual)" 2>/dev/null || true
        docker image inspect "${TARGET_IMAGE_NAME}" --format="Created: {{.Created}}" 2>/dev/null || true
        echo ""
        echo "🚀 NEXT STEPS:"
        echo "   • Run container: ./docker/linux/run.sh --compiler ${COMPILER}"
        echo "   • Use container: docker run --rm -it ${TARGET_IMAGE_NAME} /bin/bash"
    fi
    
    echo "========================================================================="
else
    echo "========================================================================="
    echo "❌ BUILD FAILED!"
    echo "========================================================================="
    echo "🔍 Check the build output above for error details"
    echo "========================================================================="
    exit 1
fi
