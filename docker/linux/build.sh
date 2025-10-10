#!/bin/bash
# ========================================================================
# 🚀 Clice Development Container Builder
# ========================================================================
# File: docker/linux/build.sh
# Purpose: Build Clice development container with all tools and dependencies
# 
# This script builds a unified development container containing all necessary
# components for the Clice development environment. The container is ready
# to use immediately with all tools pre-installed and configured.
# ========================================================================

set -e

# ========================================================================
# 🔧 Environment Setup
# ========================================================================

# Save original working directory and switch to project root
ORIG_PWD="$(pwd)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}/../.."
PROJECT_ROOT="$(pwd)"

trap 'cd "${ORIG_PWD}"' EXIT

# ========================================================================
# ⚙️ Default Configuration
# ========================================================================

COMPILER="clang"
DOCKERFILE_PATH="docker/linux/Dockerfile"
BUILD_STAGE="expanded-image"  # Always build development image (auto-expand from release if needed)
CACHE_FROM=""
CACHE_TO=""
VERSION="latest"  # Will be replaced with actual clice version in releases
REBUILD="false"

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
  --help, -h                 Show this help message

EXAMPLES:
  $0                           Build development container with clang
  $0 --compiler gcc            Build development container with gcc  
  $0 --stage packed-image     Build only the release image
  $0 --stage expanded-image Expand release image to development image
  $0 --version v1.0.0          Build versioned container (v1.0.0)
  $0 --rebuild                 Force rebuild existing image
  $0 --cache-from clice-io/clice-dev:cache  Use cache from existing image
  $0 --cache-to type=registry,ref=myregistry/myimage:cache   Push cache

VERSION-AWARE BUILDING:
  When building expanded-image with --version:
  • First checks for existing release image: clice-io/clice:linux-clang-v1.0.0
  • If found, builds development image from that release image
  • If not found, builds full multi-stage (release + development)
  • Development image will be tagged as: clice-io/clice:linux-clang-v1.0.0-expanded

BUILD MODES:
  • Multi-stage build: Builds both release and development images
  • Single-stage build: Builds only the specified stage
  • Auto-expansion: Development image can build from existing release image

The container includes:
  • Custom toolchain (fully installed and ready)
  • All development dependencies  
  • Complete development environment
  • Version-aware release image support
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
        -h|--help)
            usage; exit 0;;
        *)
            echo "❌ Unknown parameter: $1" >&2; usage; exit 1;;
    esac
done

# ========================================================================
# 🏷️ Image Naming
# ========================================================================

# Container image tag with version
IMAGE_TAG="linux-${COMPILER}-${VERSION}"
PACKED_IMAGE_NAME="clice-io/clice:${IMAGE_TAG}"

# Set the target image name based on build stage
if [ "$BUILD_STAGE" = "packed-image" ]; then
    TARGET_IMAGE_NAME="$PACKED_IMAGE_NAME"
else
    TARGET_IMAGE_NAME="clice-io/clice:${IMAGE_TAG}-expanded"
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
# 🛠️ Docker Build Arguments
# ========================================================================

BUILD_ARGS=(
    "--progress=plain"
    "--target=${BUILD_STAGE}"
    "--build-arg=COMPILER=${COMPILER}"
    "--build-arg=PACKED_IMAGE_NAME=${PACKED_IMAGE_NAME}"
    "--build-arg=BUILDKIT_INLINE_CACHE=1"  # Enable inline cache
)

# Add cache configuration with logging
if [ -n "$CACHE_FROM" ]; then
    echo "💾 Configuring cache source: ${CACHE_FROM}"
    BUILD_ARGS+=("--cache-from=${CACHE_FROM}")
fi

if [ -n "$CACHE_TO" ]; then
    echo "💾 Configuring cache destination: ${CACHE_TO}"
    BUILD_ARGS+=("--cache-to=${CACHE_TO}")
    # Log cache operations
    echo "📝 Cache operations will be logged during build"
fi

# ========================================================================
# 🏗️ Execute Build
# ========================================================================

echo "🏗️ Starting Docker build process with parallel optimization..."
echo "🔨 Build command: docker buildx build ${BUILD_ARGS[*]} -t ${TARGET_IMAGE_NAME} -f ${DOCKERFILE_PATH} ."

# ========================================================================
# 🔄 Auto-Expansion Logic (Release → Development)
# ========================================================================

# Build the target image
echo "🔍 Checking for target image: ${TARGET_IMAGE_NAME}"

if [ "$REBUILD" = "true" ] || ! docker image inspect "${TARGET_IMAGE_NAME}" >/dev/null 2>&1; then
    if [ "$REBUILD" = "true" ]; then
        echo "🔄 Force rebuilding ${BUILD_STAGE}..."
    else
        echo "🔍 Target image not found, building ${BUILD_STAGE}..."
    fi
    
    # Set up build arguments based on the target stage
    if [ "$BUILD_STAGE" = "expanded-image" ]; then
        # For development image, check if we can build from existing release image
        if docker image inspect "${PACKED_IMAGE_NAME}" >/dev/null 2>&1; then
            echo "📦 Found existing release image: ${PACKED_IMAGE_NAME}"
            echo "🏗️ Building development image from existing release image..."
            ACTUAL_RELEASE_BASE="${PACKED_IMAGE_NAME}"
        else
            echo "🔍 Release image not found: ${PACKED_IMAGE_NAME}"
            echo "🏗️ Building full multi-stage build (release + development)..."
            ACTUAL_RELEASE_BASE="packed-image"
        fi
    else
        # For release image or other stages, use default stage reference
        ACTUAL_RELEASE_BASE="packed-image"
    fi
    
    # Rebuild BUILD_ARGS with correct release base image
    BUILD_ARGS=(
        "--progress=plain"
        "--target=${BUILD_STAGE}"
        "--build-arg=COMPILER=${COMPILER}"
        "--build-arg=VERSION=${VERSION}"
        "--build-arg=RELEASE_BASE_IMAGE=${ACTUAL_RELEASE_BASE}"
        "--build-arg=BUILDKIT_INLINE_CACHE=1"
    )
    
    # Add cache configuration
    if [ -n "$CACHE_FROM" ]; then
        BUILD_ARGS+=("--cache-from=${CACHE_FROM}")
    fi
    
    if [ -n "$CACHE_TO" ]; then
        BUILD_ARGS+=("--cache-to=${CACHE_TO}")
        echo "📝 Starting build with cache-to logging enabled..."
    fi

    echo "🏗️ Building ${BUILD_STAGE} with auto-expansion support..."
    docker buildx build "${BUILD_ARGS[@]}" -t "${TARGET_IMAGE_NAME}" -f "${DOCKERFILE_PATH}" .
    
    # Log cache operations if cache-to was used  
    if [ -n "$CACHE_TO" ]; then
        echo "💾 Cache operations completed. Cache pushed to: ${CACHE_TO}"
    fi
else
    echo "✅ Target image already exists: ${TARGET_IMAGE_NAME}"
    echo "ℹ️ Use --rebuild to force rebuild"
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
        echo "   • Test container: docker run --rm -it ${TARGET_IMAGE_NAME} /bin/bash"
        echo "   • Development environment is ready to use immediately"
    fi
    
    echo "========================================================================="
else
    echo "========================================================================="
    echo "❌ BUILD FAILED!"
    echo "========================================================================="
    echo "🔍 Check the build output above for error details"
    echo "💡 Common issues:"
    echo "   • Network connectivity problems"
    echo "   • Insufficient disk space"
    echo "   • Docker daemon not running"
    echo "   • Invalid build arguments"
    echo "========================================================================="
    exit 1
fi
