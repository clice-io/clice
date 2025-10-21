# check=skip=InvalidDefaultArgInFrom,experimental=all

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
ARG CLICE_WORKDIR=/clice

# Global config shared in multi-stage builds
ARG RELEASE_PACKAGE_DIR=/clice-dev-container-package
ARG PACKED_RELEASE_PACKAGE_PATH=/release-pkg.tar.xz
ARG ENVIRONMENT_CONFIG_FILE=/root/.bashrc
ARG BUILD_CACHE_DIR=/var/cache/clice-dev-container

# APT system paths configuration
ARG APT_CACHE_DIR=/var/cache/apt
ARG APT_STATE_CACHE_DIR=/var/lib/apt

# UV docker layer cache configuration
ARG UV_PACKAGE_DIR_NAME=uv-package
ARG UV_TARBALL_DIR_NAME=tarball

# Python build scripts communicate via these environment variables
ARG PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES="\
CLICE_WORKDIR=${CLICE_WORKDIR} \
RELEASE_PACKAGE_DIR=${RELEASE_PACKAGE_DIR} \
PACKED_RELEASE_PACKAGE_PATH=${PACKED_RELEASE_PACKAGE_PATH} \
ENVIRONMENT_CONFIG_FILE=${ENVIRONMENT_CONFIG_FILE} \
BUILD_CACHE_DIR=${BUILD_CACHE_DIR}"

# ========================================================================
# 🐍 Base Stage: Python Environment Foundation
# ========================================================================
FROM ubuntu:24.04 AS base-python-environment-for-build
LABEL description="Base image with consistent Python and uv environment for builder stages"

ARG CLICE_WORKDIR
ARG APT_CACHE_DIR
ARG APT_STATE_CACHE_DIR
ARG RELEASE_PACKAGE_DIR
ARG BUILD_CACHE_DIR
ARG UV_PACKAGE_DIR_NAME
ARG UV_TARBALL_DIR_NAME

# Environment setup
ENV PATH="/root/.local/bin:${PATH}"
ENV DEBIAN_FRONTEND=noninteractive

# Do NOT copy all config at once, or all stages would be rebuilt when any file changes
# Only copy what is needed for this stage
COPY docker/linux/utility/pyproject.toml ${CLICE_WORKDIR}/docker/linux/utility/pyproject.toml
COPY config/default-toolchain-version.json ${CLICE_WORKDIR}/config/default-toolchain-version.json

# Install minimal system dependencies and uv
RUN --mount=type=cache,target=${APT_CACHE_DIR},sharing=locked \
    --mount=type=cache,target=${APT_STATE_CACHE_DIR},sharing=locked \
    --mount=type=cache,target=${BUILD_CACHE_DIR},sharing=locked \
    bash -eux - <<'SCRIPT'
    set -e
    
    # Disable auto cleanup to keep apt cache
    # This option would override Binary::apt::APT::Keep-Downloaded-Packages
    rm -f /etc/apt/apt.conf.d/docker-clean
    # It is strange that apt will accept APT::Keep-Downloaded-Packages in commandline,
    # but Binary::apt::APT::Keep-Downloaded-Packages in config file
    echo 'APT::Keep-Downloaded-Packages "true";' > /etc/apt/apt.conf.d/99keepcache
    echo 'Binary::apt::APT::Keep-Downloaded-Packages "true";' >> /etc/apt/apt.conf.d/99keepcache

    apt update -o DPkg::Lock::Timeout=-1
    apt install -y --no-install-recommends -o DPkg::Lock::Timeout=-1 curl jq ca-certificates
    
    # Get uv version from configuration
    UV_VERSION=$(jq -r .uv ${CLICE_WORKDIR}/config/default-toolchain-version.json)
    echo "📦 Installing uv version: $UV_VERSION"
    
    # Determine architecture for uv standalone build
    ARCH=$(uname -m)
    case "$ARCH" in
        x86_64)
            UV_PLATFORM="x86_64-unknown-linux-gnu"
            ;;
        *)
            echo "Unsupported architecture: $ARCH"
            exit 1
            ;;
    esac
    
    # Download uv standalone build from GitHub releases to package directory
    UV_TARBALL_NAME="uv-${UV_PLATFORM}.tar.gz"
    UV_URL="https://github.com/astral-sh/uv/releases/download/${UV_VERSION}/${UV_TARBALL_NAME}"
    
    # Create cache directory for UV tarball (Docker layer cache)
    UV_BUILD_CACHE_DIR="${BUILD_CACHE_DIR}/uv-${UV_VERSION}"
    UV_BUILD_CACHE_PACKAGE_DIR="${UV_BUILD_CACHE_DIR}/${UV_PACKAGE_DIR_NAME}"
    UV_BUILD_CACHE_TARBALL_DIR="${UV_BUILD_CACHE_DIR}/${UV_TARBALL_DIR_NAME}"
    UV_BUILD_CACHE_TARBALL_FILE="${UV_BUILD_CACHE_TARBALL_DIR}/${UV_TARBALL_NAME}"
    mkdir -p "${UV_BUILD_CACHE_TARBALL_DIR}"
    
    # Create package directory for UV
    UV_PACKAGE_ROOT="${RELEASE_PACKAGE_DIR}/uv-${UV_VERSION}"
    UV_PACKAGE_CACHE_DIR="${UV_PACKAGE_ROOT}/${UV_PACKAGE_DIR_NAME}"
    UV_PACKAGE_TARBALL_DIR="${UV_PACKAGE_ROOT}/${UV_TARBALL_DIR_NAME}"
    UV_PACKAGE_TARBALL_FILE="${UV_PACKAGE_TARBALL_DIR}/${UV_TARBALL_NAME}"
    mkdir -p "${UV_PACKAGE_CACHE_DIR}" "${UV_PACKAGE_TARBALL_DIR}"
    
    echo "🌐 Downloading uv from: $UV_URL"
    echo "💾 Cache location: ${UV_BUILD_CACHE_TARBALL_FILE}"
    echo "📦 Package location: ${UV_PACKAGE_TARBALL_FILE}"
        
    # Download to cache
    echo "⬇️  Downloading uv to cache..."
    curl -fsSL "$UV_URL" -o "$UV_BUILD_CACHE_TARBALL_FILE"

    # Copy to package directory for later packaging
    echo "📋 Copying to package directory..."
    cp "$UV_BUILD_CACHE_TARBALL_FILE" "$UV_PACKAGE_TARBALL_FILE"
    
    # Extract and install uv from package
    echo "🔧 Installing uv..."
    mkdir -p /root/.local/bin
    tar -xzf "$UV_PACKAGE_TARBALL_FILE" -C /root/.local/bin --strip-components=1
    
    # Verify installation
    echo "✅ UV installed successfully:"
    uv --version

    # Save UV version to a file for expanded-image stage to read
    # This allows expanded-image to find UV package directory without globbing
    echo "$UV_VERSION" > "${RELEASE_PACKAGE_DIR}/.uv-version"
    echo "📝 Saved UV version to: ${RELEASE_PACKAGE_DIR}/.uv-version"

    # Get Python version from configuration
    PYTHON_VERSION=$(jq -r .python ${CLICE_WORKDIR}/config/default-toolchain-version.json)
    echo "🐍 Installing Python version: $PYTHON_VERSION"
    
    # Install Python using UV with package_dir as cache
    echo "🔧 Installing Python ${PYTHON_VERSION} to package directory..."
    # This creates Python installation cache in RELEASE_PACKAGE_DIR for expand-stage
    # So we use UV_PACKAGE_CACHE_DIR here
    UV_CACHE_DIR=${UV_PACKAGE_CACHE_DIR} uv python install "$PYTHON_VERSION" --default

    # Save Python version to a file for expanded-image stage to read
    # This allows expanded-image to know which Python version to install without jq
    echo "$PYTHON_VERSION" > "${UV_PACKAGE_ROOT}/.python-version"
    echo "📝 Saved Python version to: ${UV_PACKAGE_ROOT}/.python-version"

    echo "✅ Python installation cached to: ${UV_PACKAGE_CACHE_DIR}"

    # Setup Python project environment
    echo "🔧 Setting up Python project environment..."
    # cache to build cache, here we use docker/linux/utility pyproject.toml, only for build
    UV_CACHE_DIR=${UV_BUILD_CACHE_PACKAGE_DIR} uv sync --project ${CLICE_WORKDIR}/docker/linux/utility/pyproject.toml
    echo "✅ Base Python environment setup complete!"
SCRIPT

WORKDIR ${CLICE_WORKDIR}

# ========================================================================
# 🏗️ Stage 1: Compiler Toolchain Builder
# ========================================================================
FROM base-python-environment-for-build AS toolchain-builder
LABEL description="Builds custom compiler toolchain with static libstdc++ for glibc compatibility"

ARG CLICE_WORKDIR
ARG APT_CACHE_DIR
ARG APT_STATE_CACHE_DIR
ARG BUILD_CACHE_DIR
ARG PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES

COPY config/build_config.py ${CLICE_WORKDIR}/config/build_config.py
COPY config/default-toolchain-version.json ${CLICE_WORKDIR}/config/default-toolchain-version.json
COPY docker/linux/utility/build_utils.py ${CLICE_WORKDIR}/docker/linux/utility/build_utils.py
COPY docker/linux/utility/build_clice_compiler_toolchain.py ${CLICE_WORKDIR}/docker/linux/utility/build_clice_compiler_toolchain.py

# Build the custom toolchain (Python script handles all dependencies)
RUN --mount=type=cache,target=${APT_CACHE_DIR},sharing=locked,id=toolchain-builder-apt \
    --mount=type=cache,target=${APT_STATE_CACHE_DIR},sharing=locked,id=toolchain-builder-apt-state \
    --mount=type=cache,target=${BUILD_CACHE_DIR},sharing=locked,id=toolchain-builder-cache \
    bash -eux - <<'SCRIPT'
    
    # Activate Python environment
    echo "🐍 Activating Python environment..."
    source ${CLICE_WORKDIR}/docker/linux/utility/.venv/bin/activate

    echo "🔨 Building custom compiler toolchain..."
    eval ${PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES} \
    python docker/linux/utility/build_clice_compiler_toolchain.py
    echo "✅ Toolchain build complete!"
SCRIPT

# ========================================================================
# 🏗️ Stage 2: Dependencies Downloader (Parallel to Stage 1)
# ========================================================================  
FROM base-python-environment-for-build AS dependencies-downloader
LABEL description="Downloads dev-container dependencies for cache optimization"

ARG CLICE_WORKDIR
ARG APT_CACHE_DIR
ARG APT_STATE_CACHE_DIR
ARG BUILD_CACHE_DIR
ARG PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES

COPY config/build_config.py ${CLICE_WORKDIR}/config/build_config.py
COPY config/default-toolchain-version.json ${CLICE_WORKDIR}/config/default-toolchain-version.json
COPY docker/linux/utility/build_utils.py ${CLICE_WORKDIR}/docker/linux/utility/build_utils.py
COPY docker/linux/utility/download_dependencies.py ${CLICE_WORKDIR}/docker/linux/utility/download_dependencies.py

# for download python dependencies
COPY pyproject.toml ${CLICE_WORKDIR}/pyproject.toml

# Setup Python project environment and download dependencies
RUN --mount=type=cache,target=${APT_CACHE_DIR},sharing=locked,id=dependencies-downloader-apt \
    --mount=type=cache,target=${APT_STATE_CACHE_DIR},sharing=locked,id=dependencies-downloader-apt-state \
    --mount=type=cache,target=${BUILD_CACHE_DIR},sharing=locked,id=dependencies-downloader-cache \
    bash -eux - <<'SCRIPT'
    
    # Activate Python environment
    echo "🐍 Activating Python environment..."
    source ${CLICE_WORKDIR}/docker/linux/utility/.venv/bin/activate

    echo "📥 Downloading dependencies..."
    eval ${PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES} \
    python docker/linux/utility/download_dependencies.py
    echo "✅ Dependencies download complete!"
SCRIPT

# ========================================================================
# 🏗️ Stage 3: Release Package Creator
# ========================================================================
FROM base-python-environment-for-build AS image-packer
LABEL description="Merges toolchain and dependencies into final release package"

ARG CLICE_WORKDIR
ARG RELEASE_PACKAGE_DIR
ARG APT_CACHE_DIR
ARG APT_STATE_CACHE_DIR
ARG PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES

# For execution in this layer and for package in final image
COPY config/build_config.py ${CLICE_WORKDIR}/config/build_config.py
COPY config/default-toolchain-version.json ${CLICE_WORKDIR}/config/default-toolchain-version.json
COPY docker/linux/utility/build_utils.py ${CLICE_WORKDIR}/docker/linux/utility/build_utils.py

# For execution in this layer only
COPY docker/linux/utility/create_release_package.py ${CLICE_WORKDIR}/docker/linux/utility/create_release_package.py
COPY docker/linux/container-entrypoint.sh ${CLICE_WORKDIR}/docker/linux/container-entrypoint.sh

# For final packaging only
COPY docker/linux/utility/local_setup.py ${CLICE_WORKDIR}/docker/linux/utility/local_setup.py

# Copy outputs from previous stages
# Merge by RELEASE_PACKAGE_DIR structure, each component has its own directory
# No need to manually copy individual files

# UV tarball and python
COPY --from=base-python-environment-for-build ${RELEASE_PACKAGE_DIR} ${RELEASE_PACKAGE_DIR}
# static libstdc++ toolchain
COPY --from=toolchain-builder ${RELEASE_PACKAGE_DIR} ${RELEASE_PACKAGE_DIR}
# other dependencies
COPY --from=dependencies-downloader ${RELEASE_PACKAGE_DIR} ${RELEASE_PACKAGE_DIR}

# Setup Python project environment and create final release package
RUN --mount=type=cache,target=${APT_CACHE_DIR},sharing=locked,id=packed-image-apt \
    --mount=type=cache,target=${APT_STATE_CACHE_DIR},sharing=locked,id=packed-image-apt-state \
    bash -eux - <<'SCRIPT'
    
    # Activate Python environment
    echo "🐍 Activating Python environment..."
    source ${CLICE_WORKDIR}/docker/linux/utility/.venv/bin/activate
    
    # Create final release package
    echo "📦 Creating release package..."
    eval ${PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES} \
    python docker/linux/utility/create_release_package.py
    echo "✅ Release package created successfully!"
SCRIPT

# ========================================================================
# 🏗️ Stage 4: Release Package
# ========================================================================
FROM ubuntu:24.04 AS packed-image

ARG PACKED_RELEASE_PACKAGE_PATH

# Copy only the packed release package
# All scripts, configs, and .bashrc are already inside the package
COPY --from=image-packer ${PACKED_RELEASE_PACKAGE_PATH} ${PACKED_RELEASE_PACKAGE_PATH}

# ========================================================================
# 🏗️ Stage 5: Development Image (Expanded)
# ========================================================================
FROM ${PACKED_IMAGE_NAME} AS expanded-image
LABEL description="Fully expanded development image with all tools installed"

ARG CLICE_WORKDIR
ARG RELEASE_PACKAGE_DIR
ARG PACKED_RELEASE_PACKAGE_PATH
ARG PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES
ARG UV_PACKAGE_DIR_NAME
ARG UV_TARBALL_DIR_NAME

ENV PATH="/root/.local/bin:${PATH}"

# Expand the release image into a full development environment
# We don't mark here with --mount=type=cache because this is executed on clice developer environment
# clice developer do not have the cache from previous stages
RUN bash -eux - <<'SCRIPT'
    # Extract the release package first
    echo "📦 Extracting release package..."
    mkdir -p "${RELEASE_PACKAGE_DIR}"
    tar -xJf "${PACKED_RELEASE_PACKAGE_PATH}" -C "${RELEASE_PACKAGE_DIR}"
    echo "✅ Release package extracted!"
    
    # Install UV and Python from packaged files (offline installation)
    echo "📦 Installing UV from package..."
    UV_VERSION_FILE="${RELEASE_PACKAGE_DIR}/.uv-version"
    UV_VERSION=$(cat "${UV_VERSION_FILE}")    
    echo "📋 UV version: ${UV_VERSION}"
    
    UV_PACKAGE_ROOT="${RELEASE_PACKAGE_DIR}/uv-${UV_VERSION}"
    UV_PACKAGE_CACHE_DIR="${UV_PACKAGE_ROOT}/${UV_PACKAGE_DIR_NAME}"
    UV_TARBALL_PATH="${UV_PACKAGE_ROOT}/${UV_TARBALL_DIR_NAME}/uv-*.tar.gz"
    UV_INSTALL_DIR="/root/.local/bin"
    mkdir -p "${UV_INSTALL_DIR}"
    
    echo "🔧 Extracting UV tarball..."
    tar -xzf ${UV_TARBALL_PATH} -C ${UV_INSTALL_DIR} --strip-components=1
    echo "✅ UV installed successfully!"
    
    # Install Python
    PYTHON_VERSION_FILE="${UV_PACKAGE_ROOT}/.python-version"
    PYTHON_VERSION=$(cat "${PYTHON_VERSION_FILE}")
    echo "📋 Python version: ${PYTHON_VERSION}"
    
    echo "🐍 Installing Python ${PYTHON_VERSION}..."
    UV_CACHE_DIR=${UV_PACKAGE_CACHE_DIR} uv python install "${PYTHON_VERSION}" --default
    echo "✅ Python ${PYTHON_VERSION} installed successfully!"
    
    # Run local setup directly from packaged scripts (no extraction needed)
    echo "🚀 Running local setup to expand environment..."
    
    # Get path to local_setup.py in package
    CLICE_SETUP_SCRIPTS_DIR="${RELEASE_PACKAGE_DIR}/clice-setup-scripts-unknown"
    LOCAL_SETUP_SCRIPT="${CLICE_SETUP_SCRIPTS_DIR}/docker/linux/utility/local_setup.py"
        
    # Run local setup directly from package (no venv needed, using system Python)
    eval ${PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES} \
    python "${LOCAL_SETUP_SCRIPT}"
    echo "✅ Environment expansion complete!"

    # Cleanup
    echo "🧹 Cleaning up temporary files..."
    rm -f "${PACKED_RELEASE_PACKAGE_PATH}"
    echo "✅ Cleanup complete! Dev container ready! 🎉"
SCRIPT

CMD ["/bin/bash"]
