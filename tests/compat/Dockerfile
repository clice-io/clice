# The Linux compat job's image (compat-test.yml): every program the Linux
# scenarios run beside the pixi environments' (nvcc, conda's and Intel's
# compilers, xclang), so the job downloads none of them. The system's
# packages are Ubuntu 24.04's, the runner's own, and the rest are what the
# runner image ships or the scenarios need beside it, at the versions written
# here. compat-image.yml pushes it to ghcr.io/clice-io/compat-linux, tagged
# by the SHA-256 of this file.
FROM ubuntu:24.04

LABEL org.opencontainers.image.source=https://github.com/clice-io/clice
LABEL org.opencontainers.image.description="The Linux compat job of clice-io/clice runs in this image."

ARG DEBIAN_FRONTEND=noninteractive

# The runner image's compilers (GCC 12 to 14, its default 13 behind cc and
# c++, clang 16 to 18 with 18 as clang), the job's own tools (git for the
# checkout, gh for the release runs), node for the bazelisk of the
# repository's npm packages, which the Bazel scenario finds first, and the
# build systems and cross compilers the scenarios add.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git gh nodejs unzip xz-utils python3 make file pkg-config \
        gcc g++ gcc-12 g++-12 gcc-14 g++-14 \
        clang clang-16 clang-17 clang-18 clang-tools-16 clang-tools-17 clang-tools-18 \
        lld-16 lld-17 lld-18 libclang-rt-16-dev libclang-rt-17-dev libclang-rt-18-dev \
        libc++-18-dev libc++abi-18-dev \
        bear ccache meson ninja-build xmake emscripten musl-tools \
        g++-mingw-w64-x86-64 g++-riscv64-linux-gnu \
        g++-aarch64-linux-gnu g++-arm-linux-gnueabihf \
        gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
        gcc-riscv64-unknown-elf picolibc-riscv64-unknown-elf \
    && rm -rf /var/lib/apt/lists/*

# Each download is checked against the hash its publisher gives: SHA-256,
# or the SHA-1 alone Google gives for the NDK.
SHELL ["/bin/bash", "-euo", "pipefail", "-c"]
RUN fetch() { curl -fsSL --retry 5 --retry-all-errors -o "$1" "$2" && echo "$3  $1" | "${4:-sha256}sum" -c -; } \
    # The runner image's CMake, and 4.5, the first whose `import std` needs
    # no experimental gate.
    && fetch /tmp/cmake.tar.gz \
        https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz \
        5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf \
    && tar -xzf /tmp/cmake.tar.gz -C /usr/local --strip-components=1 \
    && mkdir /opt/cmake-4.5 \
    && fetch /tmp/cmake-4.5.tar.gz \
        https://github.com/Kitware/CMake/releases/download/v4.5.0/cmake-4.5.0-linux-x86_64.tar.gz \
        81f0d5b0960fb6adc8197030ffc827fba937640d51e73e39e0e9e40d88c66f6f \
    && tar -xzf /tmp/cmake-4.5.tar.gz -C /opt/cmake-4.5 --strip-components=1 \
    # ziglang.org turns CI downloads away; its community mirrors serve the
    # same tarball.
    && for mirror in https://pkg.hexops.org/zig https://zig.tilok.dev https://ziglang.freetls.fastly.net; do \
        fetch /tmp/zig.tar.xz "$mirror/zig-x86_64-linux-0.14.1.tar.xz" \
            24aeeec8af16c381934a6cd7d95c807a8cb2cf7df9fa40d359aa884195c4716c && break; \
    done \
    && mkdir /opt/zig && tar -xJf /tmp/zig.tar.xz -C /opt/zig --strip-components=1 \
    && ln -s /opt/zig/zig /usr/local/bin/zig \
    && fetch /usr/local/bin/bazelisk \
        https://github.com/bazelbuild/bazelisk/releases/download/v1.28.1/bazelisk-linux-amd64 \
        22e7d3a188699982f661cf4687137ee52d1f24fec1ec893d91a6c4d791a75de8 \
    && chmod +x /usr/local/bin/bazelisk && ln -s bazelisk /usr/local/bin/bazel \
    # The runner image's newest NDK.
    && fetch /tmp/ndk.zip \
        https://dl.google.com/android/repository/android-ndk-r30-linux.zip \
        5107f898313790e449e87eee2183d9a20602dee9 sha1 \
    && unzip -q /tmp/ndk.zip -d /opt && mv /opt/android-ndk-r30 /opt/android-ndk \
    && rm -f /tmp/*.tar.gz /tmp/*.tar.xz /tmp/*.zip

# The runner's user, whose uid owns the workspace the job mounts; as root,
# rules_python's interpreter in the Bazel scenario refuses to run. The
# scenarios find CMake 4.5 in the home directory.
RUN useradd --uid 1001 --user-group --create-home runner \
    && ln -s /opt/cmake-4.5 /home/runner/cmake-4.5 \
    && install -d -o runner -g runner /opt/bazelisk
USER runner

# Bazelisk keeps the Bazel it downloads here rather than in the home
# directory, which the job replaces with its own.
ENV BAZELISK_HOME=/opt/bazelisk
RUN USE_BAZEL_VERSION=8.3.1 bazelisk version

# The variables the runner image names its NDKs by.
ENV ANDROID_NDK_HOME=/opt/android-ndk ANDROID_NDK_LATEST_HOME=/opt/android-ndk
ENV LANG=C.UTF-8
