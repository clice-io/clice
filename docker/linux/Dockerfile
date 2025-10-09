# build with multi-stage for cache efficiency
FROM ubuntu:24.04 AS basic-tools

# allow build script to bind-mount project source into build container (host path)
ARG BUILD_SRC

# set non-interactive frontend to avoid prompts
ENV DEBIAN_FRONTEND=noninteractive
# ensure user-local bin is on PATH for non-apt installs (xmake, uv, python)
ENV PATH="/root/.local/bin:${PATH}"

# install basic tools
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    # TODO: support more cache for python, xmake installation
    # TODO: check why cache doesn't work after add-apt-repository, may we change it to cache?
bash -eux - <<'BASH'

    set -e
    apt update
    # first install minimal apt prerequisites
    # software-properties-common for add-apt-repository
    # gnupg for gpg to verify cmake installer
    # curl, git for downloading sources
    # xz-utils, unzip for extracting archives
    # make for xmake installation
    apt install -y --no-install-recommends \
        software-properties-common \
        curl \
        gnupg \
        git \
        xz-utils \
        unzip \
        make

    # gcc, llvm PPA
    add-apt-repository -y ppa:ubuntu-toolchain-r/ppa
    apt update
BASH

# Compiler stage
FROM basic-tools AS compiler-stage

# passed from build arg
ARG COMPILER

# copy instead of bind-mount, to avoid docker build cache invalidation
COPY config/default-toolchain-version /clice/config/default-toolchain-version

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    bash -eux - <<'BASH'
    set -e

    # Always install libstdc++ development files, required for both gcc and clang to link against libstdc++
    GCC_VERSION=$(grep -E '^gcc,' /clice/config/default-toolchain-version | cut -d',' -f2)
    apt install -y --no-install-recommends "libstdc++-${GCC_VERSION}-dev"

    if [ "$COMPILER" = "gcc" ]; then
        apt install -y --no-install-recommends "gcc-${GCC_VERSION}" "g++-${GCC_VERSION}"
        update-alternatives --install /usr/bin/cc cc "/usr/bin/gcc-${GCC_VERSION}" 100
        update-alternatives --install /usr/bin/gcc gcc "/usr/bin/gcc-${GCC_VERSION}" 100
        update-alternatives --install /usr/bin/c++ c++ "/usr/bin/g++-${GCC_VERSION}" 100
        update-alternatives --install /usr/bin/g++ g++ "/usr/bin/g++-${GCC_VERSION}" 100
    elif [ "$COMPILER" = "clang" ]; then
        CLANG_VERSION=$(grep -E '^clang,' /clice/config/default-toolchain-version | cut -d',' -f2)
        # install clang toolchain, libstdc++ is already installed
        apt install -y --no-install-recommends "clang-${CLANG_VERSION}" "clang-tools-${CLANG_VERSION}" "lld-${CLANG_VERSION}" "libclang-rt-${CLANG_VERSION}-dev"
        update-alternatives --install /usr/bin/clang clang "/usr/bin/clang-${CLANG_VERSION}" 100
        update-alternatives --install /usr/bin/clang++ clang++ "/usr/bin/clang++-${CLANG_VERSION}" 100
        update-alternatives --install /usr/bin/c++ c++ "/usr/bin/clang++-${CLANG_VERSION}" 100
        update-alternatives --install /usr/bin/cc cc "/usr/bin/clang-${CLANG_VERSION}" 100
        update-alternatives --install /usr/bin/ld ld "/usr/bin/lld-${CLANG_VERSION}" 100
    else
        echo "Error: Unsupported compiler '$COMPILER'. Use 'gcc' or 'clang'." >&2; exit 1
    fi
BASH

FROM compiler-stage AS build-tool-stage

ARG XMAKE_CACHE_DIR="/docker-build-cache/xmake"
ARG CMAKE_CACHE_DIR="/docker-build-cache/cmake"
ARG UV_CACHE_DIR="/var/cache/uv"

ENV XMAKE_CACHE_DIR=${XMAKE_CACHE_DIR}
ENV CMAKE_CACHE_DIR=${CMAKE_CACHE_DIR}
ENV UV_CACHE_DIR=${UV_CACHE_DIR}

COPY ./pyproject.toml /clice/pyproject.toml

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    --mount=type=cache,target=${XMAKE_CACHE_DIR},sharing=locked \
    --mount=type=cache,target=${CMAKE_CACHE_DIR},sharing=locked \
    --mount=type=cache,target=${UV_CACHE_DIR},sharing=locked \
bash -eux - <<'BASH'

install_xmake() {
    set -e

    XMAKE_VERSION=$(grep -E '^xmake,' /clice/config/default-toolchain-version | cut -d',' -f2)
    XMAKE_BASE_URL="https://github.com/xmake-io/xmake/releases/download/v${XMAKE_VERSION}"
    XMAKE_FILENAME="xmake-bundle-v${XMAKE_VERSION}.linux.x86_64"
    XMAKE_CACHED_FILE="${XMAKE_CACHE_DIR}/${XMAKE_FILENAME}"

    if [ ! -f "${XMAKE_CACHED_FILE}" ] ; then
        rm -f "${XMAKE_CACHE_DIR}/*"
        curl -fsSL --retry 3 -o "${XMAKE_CACHED_FILE}" "${XMAKE_BASE_URL}/${XMAKE_FILENAME}"
    fi

    XMAKE_INSTALL_DIR="/usr/bin"
    XMAKE_INSTALLED_FILE="${XMAKE_INSTALL_DIR}/${XMAKE_FILENAME}"

    cp "${XMAKE_CACHED_FILE}" "${XMAKE_INSTALLED_FILE}"
    chmod +x "${XMAKE_INSTALLED_FILE}"

    update-alternatives --install /usr/bin/xmake xmake "${XMAKE_INSTALLED_FILE}" 100

    echo "export XMAKE_ROOT=y" >> ~/.bashrc
}

# Attention: DO NOT install cmake via PPA with apt, which would have to install required build-essential compiler tool chain
# We SHOULD NOT install another compiler toolchain, which could cause a lot trouble
# And we should not install compiler toolchain away from compiler stage
# So we install cmake from official installer script, and cache the downloaded files
install_cmake() {
    set -e

    # cached downloads live under /docker-build-cache/cmake (BuildKit cache mount)
    CMAKE_VERSION=$(grep -E '^cmake,' /clice/config/default-toolchain-version | cut -d',' -f2)
    ARCH="x86_64"

    BASE_URL="https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}"
    INSTALLER_FILENAME="cmake-${CMAKE_VERSION}-linux-${ARCH}.sh"
    SHA_FILENAME="cmake-${CMAKE_VERSION}-SHA-256.txt"
    ASC_FILENAME="${SHA_FILENAME}.asc"

    INSTALLER_PATH="${CMAKE_CACHE_DIR}/${INSTALLER_FILENAME}"
    SHA_PATH="${CMAKE_CACHE_DIR}/${SHA_FILENAME}"
    ASC_PATH="${CMAKE_CACHE_DIR}/${ASC_FILENAME}"

    verify_cmake_installer() {
        if ! gpg --verify "${ASC_PATH}" "${SHA_PATH}"; then
            echo "Signature verification failed for ${SHA_FILENAME}." >&2
            return 1
        fi

        local expected_hash
        expected_hash=$(grep "${INSTALLER_FILENAME}" "${SHA_PATH}" | awk '{print $1}')

        local actual_hash
        actual_hash=$(sha256sum "${INSTALLER_PATH}" | awk '{print $1}')
        if [ "${expected_hash}" != "${actual_hash}" ]; then
            echo "Checksum mismatch for ${INSTALLER_FILENAME}." >&2
            return 1
        fi

        echo "Checksum for ${INSTALLER_FILENAME} is valid."
        return 0
    }

    gpg --keyserver keys.openpgp.org --recv-keys C6C265324BBEBDC350B513D02D2CEF1034921684

    if [ ! -f "${INSTALLER_PATH}" ] || ! verify_cmake_installer; then
        rm -f "${CMAKE_CACHE_DIR}/*"

        curl -fsSL --retry 3 -o "${INSTALLER_PATH}" "${BASE_URL}/${INSTALLER_FILENAME}"
        curl -fsSL --retry 3 -o "${SHA_PATH}" "${BASE_URL}/${SHA_FILENAME}"
        curl -fsSL --retry 3 -o "${ASC_PATH}" "${BASE_URL}/${ASC_FILENAME}"

        if ! verify_cmake_installer; then
            echo "Verification of the downloaded installer failed. Cleaning cache." >&2
            rm -f "${CMAKE_CACHE_DIR}/*"
            exit 1
        fi
    fi

    sh "${INSTALLER_PATH}" --skip-license --prefix=/usr/local
}

install_python() {
    set -e
    PYTHON_VERSION=$(grep -E '^python,' /clice/config/default-toolchain-version | cut -d',' -f2)
    curl -LsSf https://astral.sh/uv/install.sh | sh
    uv python install "${PYTHON_VERSION}"
    uv sync
}

do_install() {
    set -e

    cd /clice

    export PATH="/root/.local/bin:${PATH}"
    echo "export XMAKE_ROOT=y" >> ~/.bashrc

    install_cmake &
    install_xmake &
    install_python &

    for job in $(jobs -p); do
        wait $job || exit 1
    done
}

do_install

BASH

# download compile dependencies
FROM build-tool-stage AS dependency-cache-stage

# passed from build arg
# "lto" or "non_lto"
ARG BUILD_SRC
# ARG LTO_TYPE=""

RUN --mount=type=bind,src=./,target=/clice,rw \
bash -eux - <<'BASH'

# cache_xmake_packages() {
#     set -e

#     export PATH="/root/.local/bin:${PATH}"
#     export XMAKE_ROOT=y

#     LTO_FLAG=""
#     if [ "$LTO_TYPE" = "lto" ]; then
#         LTO_FLAG="--release"
#     fi

#     xmake f -y -v --mode=release ${LTO_FLAG}
#     xmake f -y -v --mode=debug ${LTO_FLAG}
# }

do_prepare_dependency() {
    set -e

    cd /clice

    # cache_xmake_packages &

    for job in $(jobs -p); do
        wait $job || exit 1
    done
}

do_prepare_dependency

BASH

FROM dependency-cache-stage AS final

RUN bash -eux - <<'BASH'
set -e
    # clice is mounted here, so remove everything to reduce image size
    rm -rf /clice

    # disable git exception in cmake build when Fetch-Content
    git config --global --add safe.directory '*'
BASH

WORKDIR /clice

CMD ["/bin/bash"]
