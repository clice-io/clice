# check=skip=InvalidDefaultArgInFrom,experimental=all

# ========================================================================
# Clice Dev Container Multi-Stage Build System
# ========================================================================

# Arguments passed from docker image build system
ARG COMPILER
ARG PACKED_IMAGE_NAME

# Global config shared in multi-stage builds
ARG CLICE_DEV_CONTAINER_BASE_IMAGE=ubuntu:24.04
ARG CLICE_WORKDIR=/clice
ARG RELEASE_PACKAGE_DIR=/clice-dev-container-package
ARG PACKED_RELEASE_PACKAGE_PATH=/release-pkg.7z.run
ARG ENVIRONMENT_CONFIG_FILE=/root/.bashrc
ARG BUILD_CACHE_DIR=/var/cache/clice-dev-container
ARG SETUP_SCRIPTS_DIR=${RELEASE_PACKAGE_DIR}/setup_scripts-unknown

# APT system paths configuration
ARG APT_CACHE_DIR=/var/cache/apt
ARG APT_STATE_CACHE_DIR=/var/lib/apt

# UV docker layer cache configuration
ARG UV_PACKAGE_DIR_NAME=uv-packages
ARG UV_TARBALL_DIR_NAME=tarball

# Python build scripts communicate via these environment variables
ARG PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES="\
COMPILER=${COMPILER} \
CLICE_WORKDIR=${CLICE_WORKDIR} \
RELEASE_PACKAGE_DIR=${RELEASE_PACKAGE_DIR} \
PACKED_RELEASE_PACKAGE_PATH=${PACKED_RELEASE_PACKAGE_PATH} \
ENVIRONMENT_CONFIG_FILE=${ENVIRONMENT_CONFIG_FILE} \
BUILD_CACHE_DIR=${BUILD_CACHE_DIR}"

# ========================================================================
# 🐍 Base Stage: Python Environment Foundation
# ========================================================================
FROM ${CLICE_DEV_CONTAINER_BASE_IMAGE} AS base-python-environment-for-build
LABEL description="Base image with Python and uv environment for builder stages"

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
COPY docker/linux/utility/pyproject.toml                       ${CLICE_WORKDIR}/docker/linux/utility/pyproject.toml
COPY config/docker_build_stages/default-toolchain-version.json ${CLICE_WORKDIR}/config/docker_build_stages/default-toolchain-version.json

# Install minimal system dependencies, uv, and Python
RUN --mount=type=cache,target=${APT_CACHE_DIR},sharing=locked,id=python-build-env-apt \
    --mount=type=cache,target=${APT_STATE_CACHE_DIR},sharing=locked,id=python-build-env-apt-state \
    --mount=type=cache,target=${BUILD_CACHE_DIR},sharing=locked,id=python-build-env-cache \
    bash -eux - <<'SCRIPT'
    set -e
    
    # Disable APT auto cleanup to keep apt cache
    # This option would override Binary::apt::APT::Keep-Downloaded-Packages
    rm -f /etc/apt/apt.conf.d/docker-clean
    # It is strange that apt will accept APT::Keep-Downloaded-Packages in commandline,
    # but Binary::apt::APT::Keep-Downloaded-Packages in config file
    echo 'APT::Keep-Downloaded-Packages "true";' > /etc/apt/apt.conf.d/99keepcache
    echo 'Binary::apt::APT::Keep-Downloaded-Packages "true";' >> /etc/apt/apt.conf.d/99keepcache

    apt update -o DPkg::Lock::Timeout=-1
    apt install -y --no-install-recommends -o DPkg::Lock::Timeout=-1 curl jq ca-certificates software-properties-common
    # Python PPA
    add-apt-repository ppa:deadsnakes/ppa
    apt update -o DPkg::Lock::Timeout=-1
    
    # Get Python version from configuration and install via APT
    PYTHON_VERSION=$(jq -r .python ${CLICE_WORKDIR}/config/docker_build_stages/default-toolchain-version.json)
    # Get uv version from configuration
    UV_VERSION=$(jq -r .uv ${CLICE_WORKDIR}/config/docker_build_stages/default-toolchain-version.json)

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
    
    # Create package directory for UV, to be copied to release package
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
    
    echo "🐍 Installing Python ${PYTHON_VERSION} via APT..."
    apt install -y --no-install-recommends -o DPkg::Lock::Timeout=-1 "python${PYTHON_VERSION}"

    # Configure UV to use system Python only
    echo "🔧 Configuring UV to use system Python (only-system mode)..."
    uv python install "$PYTHON_VERSION" --default --python-preference only-system
    
    # Save Python version to a file for expanded-image stage to read
    echo "$PYTHON_VERSION" > "${UV_PACKAGE_ROOT}/.python-version"
    echo "📝 Saved Python version to: ${UV_PACKAGE_ROOT}/.python-version"

    # Setup Python project environment using system Python
    echo "🔧 Setting up Python project environment with system Python..."
    # UV will use the APT-installed Python (only-system mode)
    UV_CACHE_DIR=${UV_BUILD_CACHE_PACKAGE_DIR} \
        uv sync --project ${CLICE_WORKDIR}/docker/linux/utility/pyproject.toml
    echo "✅ Base Python environment setup complete (using APT Python)!"
SCRIPT

WORKDIR ${CLICE_WORKDIR}

# ========================================================================
# 🏗️ Stage 1: Compiler Toolchain Builder
# ========================================================================
FROM base-python-environment-for-build AS toolchain-builder
LABEL description="Builds custom compiler toolchain"

ARG COMPILER
ARG CLICE_WORKDIR
ARG APT_CACHE_DIR
ARG APT_STATE_CACHE_DIR
ARG BUILD_CACHE_DIR
ARG PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES

COPY config/docker_build_stages/common.py                      ${CLICE_WORKDIR}/config/docker_build_stages/common.py
COPY config/docker_build_stages/default-toolchain-version.json ${CLICE_WORKDIR}/config/docker_build_stages/default-toolchain-version.json
COPY config/docker_build_stages/toolchain_config.py            ${CLICE_WORKDIR}/config/docker_build_stages/toolchain_config.py
COPY docker/linux/utility/build_utils.py                       ${CLICE_WORKDIR}/docker/linux/utility/build_utils.py
COPY docker/linux/utility/build_clice_compiler_toolchain.py    ${CLICE_WORKDIR}/docker/linux/utility/build_clice_compiler_toolchain.py

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
LABEL description="Downloads dev-container dependencies"

ARG COMPILER
ARG CLICE_WORKDIR
ARG APT_CACHE_DIR
ARG APT_STATE_CACHE_DIR
ARG BUILD_CACHE_DIR
ARG PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES

COPY config/docker_build_stages/common.py                      ${CLICE_WORKDIR}/config/docker_build_stages/common.py
COPY config/docker_build_stages/default-toolchain-version.json ${CLICE_WORKDIR}/config/docker_build_stages/default-toolchain-version.json
COPY config/docker_build_stages/dependencies_config.py         ${CLICE_WORKDIR}/config/docker_build_stages/dependencies_config.py
COPY docker/linux/utility/build_utils.py                       ${CLICE_WORKDIR}/docker/linux/utility/build_utils.py
COPY docker/linux/utility/download_dependencies.py             ${CLICE_WORKDIR}/docker/linux/utility/download_dependencies.py

# for download python dependencies
COPY tests/pyproject.toml ${CLICE_WORKDIR}/tests/pyproject.toml

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

ARG COMPILER
ARG CLICE_WORKDIR
ARG RELEASE_PACKAGE_DIR
ARG APT_CACHE_DIR
ARG APT_STATE_CACHE_DIR
ARG BUILD_CACHE_DIR
ARG PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES
ARG SETUP_SCRIPTS_DIR

# For execution in this layer only
COPY config/docker_build_stages/common.py                      ${CLICE_WORKDIR}/config/docker_build_stages/common.py
COPY config/docker_build_stages/default-toolchain-version.json ${CLICE_WORKDIR}/config/docker_build_stages/default-toolchain-version.json
COPY config/docker_build_stages/toolchain_config.py            ${CLICE_WORKDIR}/config/docker_build_stages/toolchain_config.py
COPY config/docker_build_stages/dependencies_config.py         ${CLICE_WORKDIR}/config/docker_build_stages/dependencies_config.py
COPY config/docker_build_stages/package_config.py              ${CLICE_WORKDIR}/config/docker_build_stages/package_config.py
COPY docker/linux/utility/container-entrypoint.sh              ${CLICE_WORKDIR}/docker/linux/utility/container-entrypoint.sh
COPY docker/linux/utility/build_utils.py                       ${CLICE_WORKDIR}/docker/linux/utility/build_utils.py
COPY docker/linux/utility/create_release_package.py            ${CLICE_WORKDIR}/docker/linux/utility/create_release_package.py

# For package (setup scripts)
COPY config/docker_build_stages/common.py                      ${SETUP_SCRIPTS_DIR}/config/docker_build_stages/common.py
COPY config/docker_build_stages/default-toolchain-version.json ${SETUP_SCRIPTS_DIR}/config/docker_build_stages/default-toolchain-version.json
COPY config/docker_build_stages/dependencies_config.py         ${SETUP_SCRIPTS_DIR}/config/docker_build_stages/dependencies_config.py
COPY config/docker_build_stages/toolchain_config.py            ${SETUP_SCRIPTS_DIR}/config/docker_build_stages/toolchain_config.py
COPY config/docker_build_stages/package_config.py              ${SETUP_SCRIPTS_DIR}/config/docker_build_stages/package_config.py
COPY docker/linux/utility/build_utils.py                       ${SETUP_SCRIPTS_DIR}/docker/linux/utility/build_utils.py
COPY docker/linux/utility/local_setup.py                       ${SETUP_SCRIPTS_DIR}/docker/linux/utility/local_setup.py

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
RUN --mount=type=cache,target=${APT_CACHE_DIR},sharing=locked,id=image-packer-apt \
    --mount=type=cache,target=${APT_STATE_CACHE_DIR},sharing=locked,id=image-packer-apt-state \
    --mount=type=cache,target=${BUILD_CACHE_DIR},sharing=locked,id=dependencies-downloader-cache \
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
FROM ${CLICE_DEV_CONTAINER_BASE_IMAGE} AS packed-image

ARG CLICE_WORKDIR
ARG PACKED_RELEASE_PACKAGE_PATH

# Copy only the packed release package
# All scripts, configs, and .bashrc are already inside the package
COPY --from=image-packer ${PACKED_RELEASE_PACKAGE_PATH} ${PACKED_RELEASE_PACKAGE_PATH}

# Copy build scripts and Dockerfile
# These are needed for the final image
# Instead of using local build.sh and Dockerfile, we use the version packed here
# So we could make breaking changes to build scripts without breaking released images
COPY docker/linux/utility/common.sh ${CLICE_WORKDIR}/docker/linux/utility/common.sh
COPY docker/linux/build.sh          ${CLICE_WORKDIR}/docker/linux/build.sh
COPY docker/linux/Dockerfile        ${CLICE_WORKDIR}/docker/linux/Dockerfile

# ========================================================================
# 🏗️ Stage 5: Development Image (Expanded)
# ========================================================================
FROM ${PACKED_IMAGE_NAME} AS expanded-image
LABEL description="Fully expanded development image"

ARG COMPILER
ARG CLICE_WORKDIR
ARG RELEASE_PACKAGE_DIR
ARG PACKED_RELEASE_PACKAGE_PATH
ARG PYTHON_BUILD_SCRIPT_BASE_ENV_VARIABLES
ARG UV_PACKAGE_DIR_NAME
ARG UV_TARBALL_DIR_NAME
ARG SETUP_SCRIPTS_DIR

ENV PATH="/root/.local/bin:${PATH}"

# Expand the release image into a full development environment
# We don't mark here with --mount=type=cache because here is executed on clice developer environment
# clice developer do not have the cache from previous stages
RUN bash -eux - <<'SCRIPT'
    # Extract self-extracting 7z SFX package (no p7zip-full required)
    echo "📦 Extracting self-extracting release package..."
    mkdir -p "${RELEASE_PACKAGE_DIR}"
    
    # Make the SFX archive executable and run it
    chmod +x "${PACKED_RELEASE_PACKAGE_PATH}"
    
    # Run SFX to extract to RELEASE_PACKAGE_DIR
    # -o: output directory
    # -y: assume yes for all prompts
    "${PACKED_RELEASE_PACKAGE_PATH}" -o"${RELEASE_PACKAGE_DIR}" -y
    
    echo "✅ Release package extracted!"
    
    # Install UV and Python from packaged files (offline installation)
    echo "📦 Installing UV from package..."
    
    # Find UV version from directory name (uv-*)
    UV_PACKAGE_ROOT=$(find "${RELEASE_PACKAGE_DIR}" -maxdepth 1 -type d -name "uv-*" | head -n 1)
    
    # Extract version from directory name (e.g., uv-0.9.2 -> 0.9.2)
    UV_VERSION=$(basename "$UV_PACKAGE_ROOT" | sed 's/^uv-//')
    echo "📋 UV version: ${UV_VERSION}"
    echo "📁 UV package root: ${UV_PACKAGE_ROOT}"
    
    UV_PACKAGE_CACHE_DIR="${UV_PACKAGE_ROOT}/${UV_PACKAGE_DIR_NAME}"
    UV_TARBALL_PATH="${UV_PACKAGE_ROOT}/${UV_TARBALL_DIR_NAME}/uv-*.tar.gz"
    UV_INSTALL_DIR="/root/.local/bin"
    mkdir -p "${UV_INSTALL_DIR}"
    
    echo "🔧 Extracting UV tarball..."
    tar -xzf ${UV_TARBALL_PATH} -C ${UV_INSTALL_DIR} --strip-components=1
    echo "✅ UV installed successfully!"
    
    # Install Python - read version from .python-version file in UV package
    PYTHON_VERSION_FILE="${UV_PACKAGE_ROOT}/.python-version"
    PYTHON_VERSION=$(cat "${PYTHON_VERSION_FILE}")
    echo "📋 Python version: ${PYTHON_VERSION}"
    
    echo "🐍 Installing Python ${PYTHON_VERSION}..."
    UV_CACHE_DIR=${UV_PACKAGE_CACHE_DIR} uv python install "${PYTHON_VERSION}" --default --preview-features python-install-default
    echo "✅ Python ${PYTHON_VERSION} installed successfully!"
    
    # Run local setup directly from packaged scripts (no download needed)
    echo "🚀 Running local setup to expand environment..."
    
    LOCAL_SETUP_SCRIPT="${SETUP_SCRIPTS_DIR}/docker/linux/utility/local_setup.py"
        
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
