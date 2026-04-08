#!/bin/bash
#
# Exhaustive chained PCH test: try ALL C++ standard library headers.
# Build a chain where each link adds one header on top of the previous PCH.
# Report which header (if any) breaks the chain.
#
set -uo pipefail

CLANG="${CLANG:-clang++}"
STD="${STD:--std=c++20}"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "=== Exhaustive Chained PCH Test ==="
echo "clang: $($CLANG --version | head -1)"
echo "workdir: $WORKDIR"
echo ""

# ALL C++20 standard library headers (sorted, no sub-directories)
HEADERS=(
    algorithm
    any
    array
    atomic
    barrier
    bit
    bitset
    cassert
    cctype
    cerrno
    cfenv
    cfloat
    charconv
    chrono
    cinttypes
    climits
    clocale
    cmath
    codecvt
    compare
    complex
    concepts
    condition_variable
    coroutine
    csetjmp
    csignal
    cstdarg
    cstddef
    cstdint
    cstdio
    cstdlib
    cstring
    ctime
    cuchar
    cwchar
    cwctype
    deque
    exception
    execution
    expected
    filesystem
    format
    forward_list
    fstream
    functional
    future
    initializer_list
    iomanip
    ios
    iosfwd
    iostream
    istream
    iterator
    latch
    limits
    list
    locale
    map
    memory
    memory_resource
    mutex
    new
    numbers
    numeric
    optional
    ostream
    print
    queue
    random
    ranges
    ratio
    regex
    scoped_allocator
    semaphore
    set
    shared_mutex
    source_location
    span
    spanstream
    sstream
    stack
    stdexcept
    stop_token
    streambuf
    string
    string_view
    syncstream
    system_error
    thread
    tuple
    type_traits
    typeindex
    typeinfo
    unordered_map
    unordered_set
    utility
    valarray
    variant
    vector
    version
)

NUM=${#HEADERS[@]}
echo "Total headers: $NUM"
echo ""

PREV_PCH=""
PASS=0
FAIL=0
FAIL_LIST=""
TOTAL_START=$(date +%s%N)

for i in "${!HEADERS[@]}"; do
    hdr="${HEADERS[$i]}"
    idx=$((i + 1))
    HDR_FILE="$WORKDIR/link_${idx}.h"
    PCH_FILE="$WORKDIR/link_${idx}.pch"

    echo "#include <$hdr>" > "$HDR_FILE"

    LINK_START=$(date +%s%N)
    if [ -z "$PREV_PCH" ]; then
        OUTPUT=$($CLANG -x c++-header $STD -o "$PCH_FILE" "$HDR_FILE" 2>&1)
        RC=$?
    else
        OUTPUT=$($CLANG -x c++-header $STD -include-pch "$PREV_PCH" -o "$PCH_FILE" "$HDR_FILE" 2>&1)
        RC=$?
    fi
    LINK_END=$(date +%s%N)
    LINK_MS=$(( (LINK_END - LINK_START) / 1000000 ))

    if [ $RC -eq 0 ] && [ -f "$PCH_FILE" ]; then
        SIZE=$(stat -c%s "$PCH_FILE")
        SIZE_KB=$(( SIZE / 1024 ))
        printf "  [PASS] %3d/%d  %-25s  %5dms  %5dKB\n" "$idx" "$NUM" "<$hdr>" "$LINK_MS" "$SIZE_KB"
        PREV_PCH="$PCH_FILE"
        PASS=$((PASS + 1))
    else
        printf "  [FAIL] %3d/%d  %-25s  %5dms\n" "$idx" "$NUM" "<$hdr>" "$LINK_MS"
        if [ -n "$OUTPUT" ]; then
            echo "         Error: $(echo "$OUTPUT" | head -3)"
        fi
        FAIL=$((FAIL + 1))
        FAIL_LIST="$FAIL_LIST $hdr"
        # Skip this header in the chain, continue with previous PCH
    fi
done

TOTAL_END=$(date +%s%N)
TOTAL_MS=$(( (TOTAL_END - TOTAL_START) / 1000000 ))

echo ""
echo "========================================="
echo "RESULTS"
echo "========================================="
echo "  Total headers: $NUM"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
echo "  Total time: ${TOTAL_MS}ms"
if [ $FAIL -gt 0 ]; then
    echo "  Failed headers:$FAIL_LIST"
fi
echo ""

# Final correctness check: compile a test file with the last successful PCH
if [ -n "$PREV_PCH" ] && [ -f "$PREV_PCH" ]; then
    cat > "$WORKDIR/final_test.cpp" << 'SRCEOF'
#include <type_traits>
int main() {
    static_assert(std::is_integral_v<int>);
    return 0;
}
SRCEOF
    echo "Final PCH correctness check..."
    if $CLANG $STD -include-pch "$PREV_PCH" -fsyntax-only "$WORKDIR/final_test.cpp" 2>&1; then
        echo "  Result: PASS"
    else
        echo "  Result: FAIL"
    fi
    echo "  Chain depth at end: $PASS links"
fi
