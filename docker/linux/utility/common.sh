#!/bin/bash
# ========================================================================
# Clice Development Container Common Variables
# ========================================================================

set -e

# ========================================================================
# ⚙️ Default Configuration
# ========================================================================

# These are the default values that can be overridden by command-line arguments
DEFAULT_COMPILER="clang"
DEFAULT_VERSION="latest"
DEFAULT_BUILD_STAGE="packed-image"
# where clice is located inside the docker container
CLICE_DIR="/clice"
# pwd inside the container when you open a shell
DEFAULT_CONTAINER_WORKDIR="${CLICE_DIR}"

DOCKERFILE_PATH="docker/linux/Dockerfile"

# ========================================================================
# 🏷️ Naming Convention Functions
# ========================================================================

# Generates the base image tag.
# Usage: get_image_tag <compiler> <version>
get_image_tag() {
    local compiler="$1"
    local version="$2"
    echo "linux-${compiler}-v${version}"
}

# Generates the full name for the packed (release) image.
# Usage: get_packed_image_name <compiler> <version>
get_packed_image_name() {
    local compiler="$1"
    local version="$2"
    local image_tag
    image_tag=$(get_image_tag "$compiler" "$version")
    echo "clice.io/clice:${image_tag}"
}

# Generates the full name for the expanded (development) image.
# Usage: get_expanded_image_name <compiler> <version>
get_expanded_image_name() {
    local packed_image_name
    packed_image_name=$(get_packed_image_name "$1" "$2")
    echo "${packed_image_name}-expanded"
}

# Generates the name for the development container.
# Usage: get_container_name <compiler> <version>
get_container_name() {
    local compiler="$1"
    local version="$2"
    echo "clice_dev-linux-${compiler}-v${version}"
}
