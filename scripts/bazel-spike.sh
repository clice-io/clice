#!/usr/bin/env bash
# Bazel spike checks on a box: the unit tests through bazel test, and
# build timings against CMake. MODE=stage|cold|incremental|cmake-cold|cache|ci.
set -u
# npm's bazelisk (package-lock.json), as the workflow runs it.
bazel() { npx --no bazel "$@"; }
T="//:clice //:unit_tests"

case "${MODE:-stage}" in
stage)
    bazel ${BZ_STARTUP:-} test ${BZ_FLAGS:-} //:unit_tests_test 2>&1 | tail -6
    grep -E "tests from|PASSED|FAILED|SKIPPED" bazel-testlogs/unit_tests_test/test.log | tail -5
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
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake -DCLICE_ENABLE_TEST=ON -DCLICE_CI_ENVIRONMENT=ON > /tmp/cm-cold.log 2>&1
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
ci)
    # A CI leg: a fresh runner with nothing cached, then a fresh runner that
    # restored the disk and repository caches the first one saved.
    ci=/tmp/bz-ci; rm -rf $ci; mkdir -p $ci
    caches="--disk_cache=$ci/disk --repository_cache=$ci/repo"
    leg() {
        local label=$1 base=$2
        start=$(date +%s)
        bazel ${BZ_STARTUP:-} --output_base=$base test ${BZ_FLAGS:-} $caches //:dist //:unit_tests_test > $ci/$label.log 2>&1
        local rc=$?
        echo "$label: $(( $(date +%s) - start ))s rc=$rc; $(grep -E 'processes:' $ci/$label.log)"
        bazel ${BZ_STARTUP:-} --output_base=$base shutdown > /dev/null 2>&1
    }
    leg cold $ci/base1
    du -sh $ci/disk $ci/repo
    touch $ci/mark; sleep 1
    leg restored $ci/base2
    echo "disk cache entries not read by the restored run: $(find $ci/disk -type f ! -newer $ci/mark | wc -l) of $(find $ci/disk -type f | wc -l)"
    printf '\n// ci %s\n' "$(date +%s%N)" >> src/feature/hover.cpp
    leg one-edit $ci/base3
    git checkout -- src
    tar -C $ci -cf - disk repo | zstd -T0 -3 | wc -c | awk '{printf "zstd -3 of both caches: %.0f MB\n", $1/1048576}'
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
