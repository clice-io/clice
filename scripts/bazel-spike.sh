#!/usr/bin/env bash
# Bazel spike checks on a box: build, stage like build/<type>/{bin,lib}, run
# the unit tests. MODE=stage|cold|incremental|cmake-cold|cache.
set -u
export PATH=$HOME/opt/bin:$PATH
export CLICE_LLVM_ROOT=${CLICE_LLVM_ROOT:-$(sed -n "s/^LLVM_INSTALL_PATH:PATH=//p" build/RelWithDebInfo/CMakeCache.txt)}
T="//:clice //:unit_tests"
exe=""
case "$(uname -s)" in MINGW*|MSYS*) exe=".exe" ;; incremental)
    # The same edits through both builds; both trees already built.
    edit() { printf '\n// spike %s\n' "$(date +%s%N)" >> "$1"; }
    run() {
        local label=$1
        start=$(date +%s%N)
        bazel ${BZ_STARTUP:-} build ${BZ_FLAGS:-} $T > /tmp/bz-inc.log 2>&1
        local bz=$(( ($(date +%s%N) - start) / 1000000 ))
        start=$(date +%s%N)
        cmake --build build/RelWithDebInfo-cold --target clice unit_tests > /tmp/cm-inc.log 2>&1
        local cm=$(( ($(date +%s%N) - start) / 1000000 ))
        echo "| $label | ${bz} ms ($(grep -oE '[0-9]+ processwrapper-sandbox|[0-9]+ local' /tmp/bz-inc.log | head -1)) | ${cm} ms ($(grep -c 'Building CXX' /tmp/cm-inc.log) compiles) |"
    }
    echo "| edit | bazel | cmake |"
    echo "|---|---|---|"
    run "no-op"
    edit src/feature/hover.cpp; run "one leaf .cpp (feature/hover.cpp)"
    edit src/support/logging.h; run "support/logging.h (52 direct includers)"
    git checkout -- src; run "revert"
    ;;
cache)
    # CI restoring a disk cache: fresh output base, warm --disk_cache. Then the
    # same cache from a checkout at another path (a developer machine).
    rm -rf /tmp/bz-disk
    bazel ${BZ_STARTUP:-} clean >/dev/null 2>&1
    bazel ${BZ_STARTUP:-} build ${BZ_FLAGS:-} --disk_cache=/tmp/bz-disk $T > /dev/null 2>&1
    bazel ${BZ_STARTUP:-} clean >/dev/null 2>&1
    start=$(date +%s)
    bazel ${BZ_STARTUP:-} build ${BZ_FLAGS:-} --disk_cache=/tmp/bz-disk $T > /tmp/bz-cache.log 2>&1
    echo "same path, warm disk cache: $(( $(date +%s) - start ))s; $(grep -E 'processes:' /tmp/bz-cache.log)"
    du -sh /tmp/bz-disk
    rm -rf /tmp/elsewhere && mkdir -p /tmp/elsewhere && git archive HEAD | tar -x -C /tmp/elsewhere
    cd /tmp/elsewhere
    start=$(date +%s)
    bazel ${BZ_STARTUP:-} build ${BZ_FLAGS:-} --disk_cache=/tmp/bz-disk $T > /tmp/bz-cache2.log 2>&1
    echo "other checkout path, same cache: $(( $(date +%s) - start ))s; $(grep -E 'processes:' /tmp/bz-cache2.log)"
    bazel ${BZ_STARTUP:-} shutdown >/dev/null 2>&1
    ;;
esac

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
incremental)
    # The same edits through both builds; both trees already built.
    edit() { printf '\n// spike %s\n' "$(date +%s%N)" >> "$1"; }
    run() {
        local label=$1
        start=$(date +%s%N)
        bazel ${BZ_STARTUP:-} build ${BZ_FLAGS:-} $T > /tmp/bz-inc.log 2>&1
        local bz=$(( ($(date +%s%N) - start) / 1000000 ))
        start=$(date +%s%N)
        cmake --build build/RelWithDebInfo-cold --target clice unit_tests > /tmp/cm-inc.log 2>&1
        local cm=$(( ($(date +%s%N) - start) / 1000000 ))
        echo "| $label | ${bz} ms ($(grep -oE '[0-9]+ processwrapper-sandbox|[0-9]+ local' /tmp/bz-inc.log | head -1)) | ${cm} ms ($(grep -c 'Building CXX' /tmp/cm-inc.log) compiles) |"
    }
    echo "| edit | bazel | cmake |"
    echo "|---|---|---|"
    run "no-op"
    edit src/feature/hover.cpp; run "one leaf .cpp (feature/hover.cpp)"
    edit src/support/logging.h; run "support/logging.h (52 direct includers)"
    git checkout -- src; run "revert"
    ;;
cache)
    # CI restoring a disk cache: fresh output base, warm --disk_cache. Then the
    # same cache from a checkout at another path (a developer machine).
    rm -rf /tmp/bz-disk
    bazel ${BZ_STARTUP:-} clean >/dev/null 2>&1
    bazel ${BZ_STARTUP:-} build ${BZ_FLAGS:-} --disk_cache=/tmp/bz-disk $T > /dev/null 2>&1
    bazel ${BZ_STARTUP:-} clean >/dev/null 2>&1
    start=$(date +%s)
    bazel ${BZ_STARTUP:-} build ${BZ_FLAGS:-} --disk_cache=/tmp/bz-disk $T > /tmp/bz-cache.log 2>&1
    echo "same path, warm disk cache: $(( $(date +%s) - start ))s; $(grep -E 'processes:' /tmp/bz-cache.log)"
    du -sh /tmp/bz-disk
    rm -rf /tmp/elsewhere && mkdir -p /tmp/elsewhere && git archive HEAD | tar -x -C /tmp/elsewhere
    cd /tmp/elsewhere
    start=$(date +%s)
    bazel ${BZ_STARTUP:-} build ${BZ_FLAGS:-} --disk_cache=/tmp/bz-disk $T > /tmp/bz-cache2.log 2>&1
    echo "other checkout path, same cache: $(( $(date +%s) - start ))s; $(grep -E 'processes:' /tmp/bz-cache2.log)"
    bazel ${BZ_STARTUP:-} shutdown >/dev/null 2>&1
    ;;
esac
