# ========================================================================
# 🚀 Clice Dev Container Multi-Stage Build System
# ========================================================================
# File: docker/linux/Dockerfile
# Purpose: Optimized multi-stage build for Clice development container
# 
# This Dockerfile implements Python-first build approach with minimal
# system dependencies, letting Python scripts handle dependency installation.
# ========================================================================

# Arguments passed from docker image build system
ARG COMPILER
ARG PACKED_IMAGE_NAME

# Global config shared in multi-stage builds
ARG RELEASE_PACKAGE_DIR="/clice-dev-container-package"
ARG PACKED_RELEASE_PACKAGE_PATH="/release-pkg.tar.xz"
ARG ENVIRONMENT_CONFIG_FILE="/root/.bashrc"
ARG CACHE_DIR_ROOT="/var/cache/clice-dev-container"

# APT system paths configuration
ARG APT_CACHE_DIR="/var/cache/apt"
ARG APT_STATE_CACHE_DIR="/var/lib/apt"

# UV cache configuration
ARG UV_CACHE_DIR="/var/cache/uv"

# ========================================================================
# 🐍 Base Stage: Python Environment Foundation
# ========================================================================
FROM ubuntu:24.04 AS base-python-environment
LABEL description="Base image with consistent Python and uv environment for all stages"

# Environment setup
ENV PATH="/root/.local/bin:${PATH}"
ENV UV_CACHE_DIR=${UV_CACHE_DIR}

COPY config /clice/config
COPY docker/linux /clice/docker/linux

# Install minimal system dependencies and uv
RUN --mount=type=cache,target=${APT_CACHE_DIR},sharing=locked \
    --mount=type=cache,target=${APT_STATE_CACHE_DIR},sharing=locked \
    --mount=type=cache,target=${UV_CACHE_DIR},sharing=locked \
    bash -eux - <<'SCRIPT'
    apt update
    apt install -y --no-install-recommends curl jq ca-certificates
    
    # Install uv for Python management
    curl -LsSf https://astral.sh/uv/install.sh | sh

    # Get Python version from configuration
    PYTHON_VERSION=$(jq -r .python /clice/config/default-toolchain-version.json)
    echo "Installing Python version: $PYTHON_VERSION"
    
    # Install specified Python version
    uv python install "$PYTHON_VERSION"
SCRIPT

WORKDIR /clice

# ========================================================================
# 🏗️ Stage 1: Compiler Toolchain Builder
# ========================================================================
FROM base-python-environment AS toolchain-builder
LABEL description="Builds custom compiler toolchain with static libstdc++ for glibc compatibility"


# For build_config.py
ENV CACHE_DIR_ROOT=${CACHE_DIR_ROOT}
ENV RELEASE_PACKAGE_DIR=${RELEASE_PACKAGE_DIR}

# uv auto reads from this env variable
ENV UV_CACHE_DIR=${UV_CACHE_DIR}

ENV DEBIAN_FRONTEND=noninteractive

# Copy additional project structure (config already copied in base)
COPY docker/linux /clice/docker/linux

# Build the custom toolchain (Python script handles all dependencies)
RUN --mount=type=cache,target=${APT_CACHE_DIR},sharing=locked,id=toolchain-builder-apt \
    --mount=type=cache,target=${APT_STATE_CACHE_DIR},sharing=locked,id=toolchain-builder-apt-state \
    --mount=type=cache,target=${CACHE_DIR_ROOT},sharing=locked,id=toolchain-builder-cache \
    --mount=type=cache,target=${UV_CACHE_DIR},sharing=locked,id=toolchain-builder-uv \
    bash -eux - <<'SCRIPT'
    # Setup Python project environment
    uv sync --project /clice/docker/linux/utility/pyproject.toml
    
    # Activate Python environment and build toolchain
    source /clice/docker/linux/utility/.venv/bin/activate
    python docker/linux/utility/build_clice_compiler_toolchain.py
SCRIPT

# ========================================================================
# 🏗️ Stage 2: Dependencies Downloader (Parallel to Stage 1)
# ========================================================================  
FROM base-python-environment AS dependencies-downloader
LABEL description="Downloads dev-container dependencies for cache optimization"

ENV CACHE_DIR_ROOT=${CACHE_DIR_ROOT}
ENV RELEASE_PACKAGE_DIR=${RELEASE_PACKAGE_DIR}

# uv auto reads from this env variable
ENV UV_CACHE_DIR=${UV_CACHE_DIR}

ENV DEBIAN_FRONTEND=noninteractive

# Copy additional project structure (config already copied in base)
COPY docker /clice/docker

# Setup Python project environment and download dependencies
RUN --mount=type=cache,target=${APT_CACHE_DIR},sharing=locked,id=dependencies-downloader-apt \
    --mount=type=cache,target=${APT_STATE_CACHE_DIR},sharing=locked,id=dependencies-downloader-apt-state \
    --mount=type=cache,target=${CACHE_DIR_ROOT},sharing=locked,id=dependencies-downloader-cache \
    --mount=type=cache,target=${UV_CACHE_DIR},sharing=locked,id=dependencies-downloader-uv \
    bash -eux - <<'SCRIPT'
    # Setup Python environment
    uv sync --project /clice/docker/linux/utility/pyproject.toml
    
    # Download dependencies
    source /clice/docker/linux/utility/.venv/bin/activate
    python docker/linux/utility/download_dependencies.py
SCRIPT

# ========================================================================
# 🏗️ Stage 3: Release Package Creator
# ========================================================================
FROM base-python-environment AS image-packer
LABEL description="Merges toolchain and dependencies into final release package"

# For build_config.py
ENV RELEASE_PACKAGE_DIR=${RELEASE_PACKAGE_DIR}
ENV PACKED_RELEASE_PACKAGE_PATH=${PACKED_RELEASE_PACKAGE_PATH}
ENV ENVIRONMENT_CONFIG_FILE=${ENVIRONMENT_CONFIG_FILE}

# uv auto reads from this env variable
ENV UV_CACHE_DIR=${UV_CACHE_DIR}

ENV DEBIAN_FRONTEND=noninteractive

# Copy outputs from previous stages
# Merge by RELEASE_PACKAGE_DIR structure, each component has its own directory
# No need to manually copy individual files
COPY --from=toolchain-builder ${RELEASE_PACKAGE_DIR} ${RELEASE_PACKAGE_DIR}
COPY --from=dependencies-downloader ${RELEASE_PACKAGE_DIR} ${RELEASE_PACKAGE_DIR}

# Setup Python project environment and create final release package
RUN --mount=type=cache,target=${APT_CACHE_DIR},sharing=locked,id=packed-image-apt \
    --mount=type=cache,target=${APT_STATE_CACHE_DIR},sharing=locked,id=packed-image-apt-state \
    --mount=type=cache,target=${UV_CACHE_DIR},sharing=locked,id=packed-image-uv \
    bash -eux - <<'SCRIPT'
    # Setup Python environment
    uv sync --project /clice/docker/linux/utility/pyproject.toml
    
    # Create final release package by merging stage outputs
    source /clice/docker/linux/utility/.venv/bin/activate
    python docker/linux/utility/create_release_package.py
SCRIPT

# ========================================================================
# 🏗️ Stage 4: Release Package Creator
# ========================================================================
FROM base-python-environment AS packed-image  

# Copy project configuration to determine Python version
# these two COPYs must be scheduled before expand image
# so the config and scripts could keep the same with docker image build environment
COPY config /clice/config
COPY docker/linux /clice/docker/linux

COPY --from=image-packer ${PACKED_RELEASE_PACKAGE_PATH} ${PACKED_RELEASE_PACKAGE_PATH}
COPY --from=image-packer ${ENVIRONMENT_CONFIG_FILE} ${ENVIRONMENT_CONFIG_FILE}

# ========================================================================
# 🏗️ Stage 5: Development Image (Expanded)
# ========================================================================
FROM ${PACKED_IMAGE_NAME} AS expanded-image
LABEL description="Fully expanded development image with all tools installed"

# We should NOT copy project structure for local setup
# Local config and other scripts may be different from pack environment
# The two COPYs below are copyed from pack environment, not from local
# /clice/config
# /clice/docker/linux

# Expand the release image into a full development environment
# We don't mark here with --mount=type=cache because this is executed on clice developer environment
# clice developer do not have the cache from previous stages
RUN bash -eux - <<'SCRIPT'
    
    # Setup Python project environment
    uv sync --project /clice/pyproject.toml
    
    # Run local setup to expand everything
    source /clice/docker/linux/utility/.venv/bin/activate
    python docker/linux/utility/local_setup.py

    # cleanup project structure to reduce image size
    # User could refer to ${RELEASE_PACKAGE_DIR}/manifest.json if needed
    rm -rf /clice
SCRIPT

WORKDIR /clice

CMD ["/bin/bash"]
