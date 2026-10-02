#!/usr/bin/env bash
# Bazel spike checks on a box: build, stage like build/<type>/{bin,lib}, run
# the unit tests. MODE=stage|cold|incremental|cmake-cold|cache.
set -u
export PATH=$HOME/opt/bin:$PATH
export CLICE_LLVM_ROOT=${CLICE_LLVM_ROOT:-$(sed -n "s/^LLVM_INSTALL_PATH:PATH=//p" build/RelWithDebInfo/CMakeCache.txt)}
T="//:clice //:unit_tests"
exe=""
case "$(uname -s)" in MINGW*|MSYS*) exe=".exe" ;; esac

stage() {
    local d=build-bazel
    rm -rf "$d" && mkdir -p "$d/bin" "$d/lib"
    cp "bazel-bin/clice$exe" "bazel-bin/unit_tests$exe" "$d/bin/"
    cp -r "$CLICE_LLVM_ROOT/lib/clang" "$d/lib/"
}

case "${MODE:-stage}" in
stage)
    bazel ${BZ_STARTUP:-} build ${BZ_FLAGS:-} $T 2>&1 | tail -3
    stage
    CLICE_TEST_DATA_DIR=$PWD/tests/data ./build-bazel/bin/unit_tests$exe 2>&1 | tail -15
    ;;
cold)
    # No local cache at all: the first CI run, or a fresh clone.
    bazel ${BZ_STARTUP:-} clean --expunge >/dev/null 2>&1
    start=$(date +%s)
    bazel ${BZ_STARTUP:-} build ${BZ_FLAGS:-} $T > /tmp/bz-cold.log 2>&1
    echo "bazel cold (fetch + analysis + build): $(( $(date +%s) - start ))s; $(grep -E 'processes:' /tmp/bz-cold.log)"
    ;;
cmake-cold)
    # CI's build without its compiler cache.
    rm -rf build/RelWithDebInfo-cold
    start=$(date +%s)
    CCACHE_DISABLE=1 cmake -B build/RelWithDebInfo-cold -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake -DCLICE_ENABLE_TEST=ON -DCLICE_CI_ENVIRONMENT=ON \
        -DLLVM_INSTALL_PATH="$CLICE_LLVM_ROOT" > /tmp/cm-cold.log 2>&1
    CCACHE_DISABLE=1 cmake --build build/RelWithDebInfo-cold --target clice unit_tests >> /tmp/cm-cold.log 2>&1
    echo "cmake cold (configure + build, PCH, no ccache): $(( $(date +%s) - start ))s; rc=$?"
    tail -2 /tmp/cm-cold.log
    ;;
esac
