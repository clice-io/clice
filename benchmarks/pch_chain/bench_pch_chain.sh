#!/bin/bash
#
# Benchmark: Monolithic PCH vs Chained PCH (one-include-per-link)
#
# Tests whether clang's chained PCH works correctly with 15+ chain links,
# and compares build time against a single monolithic PCH.
#
set -euo pipefail

CLANG="${CLANG:-clang++}"
STD="${STD:--std=c++20}"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "=== PCH Chain Benchmark ==="
echo "clang: $($CLANG --version | head -1)"
echo "workdir: $WORKDIR"
echo ""

# --- Standard library headers to include (order matters for chaining) ---
HEADERS=(
    "cstddef"
    "cstdint"
    "type_traits"
    "utility"
    "memory"
    "string"
    "string_view"
    "vector"
    "array"
    "map"
    "unordered_map"
    "set"
    "optional"
    "variant"
    "functional"
    "algorithm"
    "numeric"
    "iostream"
    "sstream"
    "format"
)

NUM_HEADERS=${#HEADERS[@]}
echo "Headers: $NUM_HEADERS"
echo "List: ${HEADERS[*]}"
echo ""

# ============================================================
# 1. Monolithic PCH: one file with all includes
# ============================================================

echo "========================================="
echo "1. MONOLITHIC PCH"
echo "========================================="

# Generate the preamble header
MONO_HDR="$WORKDIR/mono_preamble.h"
for hdr in "${HEADERS[@]}"; do
    echo "#include <$hdr>" >> "$MONO_HDR"
done

echo "Building monolithic PCH ($NUM_HEADERS includes)..."
MONO_START=$(date +%s%N)
$CLANG -x c++-header $STD -o "$WORKDIR/mono.pch" "$MONO_HDR" 2>&1
MONO_END=$(date +%s%N)
MONO_MS=$(( (MONO_END - MONO_START) / 1000000 ))
MONO_SIZE=$(stat -c%s "$WORKDIR/mono.pch")
echo "  Time: ${MONO_MS}ms"
echo "  Size: $(( MONO_SIZE / 1024 ))KB"

# Test: compile a source file using the monolithic PCH
cat > "$WORKDIR/test_mono.cpp" << 'SRCEOF'
int main() {
    std::vector<std::string> v = {"hello", "world"};
    std::map<int, std::string> m = {{1, "one"}, {2, "two"}};
    auto opt = std::make_optional(42);
    std::cout << v[0] << " " << m[1] << " " << *opt << std::endl;
    return 0;
}
SRCEOF

echo "  Compiling test with monolithic PCH..."
if $CLANG $STD -include-pch "$WORKDIR/mono.pch" -fsyntax-only "$WORKDIR/test_mono.cpp" 2>&1; then
    echo "  Result: PASS"
else
    echo "  Result: FAIL"
fi
echo ""

# ============================================================
# 2. Chained PCH: one PCH per include, each chaining the previous
# ============================================================

echo "========================================="
echo "2. CHAINED PCH (one include per link)"
echo "========================================="

CHAIN_TOTAL_START=$(date +%s%N)

PREV_PCH=""
for i in "${!HEADERS[@]}"; do
    hdr="${HEADERS[$i]}"
    idx=$((i + 1))
    CHAIN_HDR="$WORKDIR/chain_${idx}.h"
    CHAIN_PCH="$WORKDIR/chain_${idx}.pch"

    echo "#include <$hdr>" > "$CHAIN_HDR"

    LINK_START=$(date +%s%N)
    if [ -z "$PREV_PCH" ]; then
        # First link: no previous PCH
        if ! $CLANG -x c++-header $STD -o "$CHAIN_PCH" "$CHAIN_HDR" 2>&1; then
            echo "  FAIL at link $idx ($hdr) — build error"
            break
        fi
    else
        # Subsequent links: chain from previous PCH
        if ! $CLANG -x c++-header $STD -include-pch "$PREV_PCH" -o "$CHAIN_PCH" "$CHAIN_HDR" 2>&1; then
            echo "  FAIL at link $idx ($hdr) — build error"
            echo "  (chained PCH may have a bug at this chain depth)"
            break
        fi
    fi
    LINK_END=$(date +%s%N)
    LINK_MS=$(( (LINK_END - LINK_START) / 1000000 ))
    LINK_SIZE=$(stat -c%s "$CHAIN_PCH" 2>/dev/null || echo 0)

    echo "  Link $idx/$NUM_HEADERS: <$hdr>  ${LINK_MS}ms  $(( LINK_SIZE / 1024 ))KB"
    PREV_PCH="$CHAIN_PCH"
done

CHAIN_TOTAL_END=$(date +%s%N)
CHAIN_TOTAL_MS=$(( (CHAIN_TOTAL_END - CHAIN_TOTAL_START) / 1000000 ))

echo ""
echo "  Total chain build time: ${CHAIN_TOTAL_MS}ms"
if [ -n "$PREV_PCH" ] && [ -f "$PREV_PCH" ]; then
    LAST_SIZE=$(stat -c%s "$PREV_PCH")
    echo "  Final PCH size: $(( LAST_SIZE / 1024 ))KB"
fi
echo ""

# Test: compile the same source file using the final chained PCH
echo "  Compiling test with chained PCH (final link)..."
if [ -n "$PREV_PCH" ] && [ -f "$PREV_PCH" ]; then
    if $CLANG $STD -include-pch "$PREV_PCH" -fsyntax-only "$WORKDIR/test_mono.cpp" 2>&1; then
        echo "  Result: PASS"
    else
        echo "  Result: FAIL"
    fi
else
    echo "  Result: SKIP (chain build failed)"
fi
echo ""

# ============================================================
# 3. Incremental rebuild simulation: add one more include
# ============================================================

echo "========================================="
echo "3. INCREMENTAL REBUILD (add <chrono>)"
echo "========================================="

EXTRA_HDR="$WORKDIR/chain_extra.h"
EXTRA_PCH="$WORKDIR/chain_extra.pch"
echo "#include <chrono>" > "$EXTRA_HDR"

echo "  Monolithic: rebuild entire PCH..."
echo "#include <chrono>" >> "$MONO_HDR"
MONO_REBUILD_START=$(date +%s%N)
$CLANG -x c++-header $STD -o "$WORKDIR/mono.pch" "$MONO_HDR" 2>&1
MONO_REBUILD_END=$(date +%s%N)
MONO_REBUILD_MS=$(( (MONO_REBUILD_END - MONO_REBUILD_START) / 1000000 ))
echo "  Monolithic rebuild: ${MONO_REBUILD_MS}ms"

if [ -n "$PREV_PCH" ] && [ -f "$PREV_PCH" ]; then
    echo "  Chained: append one link..."
    INCR_START=$(date +%s%N)
    if $CLANG -x c++-header $STD -include-pch "$PREV_PCH" -o "$EXTRA_PCH" "$EXTRA_HDR" 2>&1; then
        INCR_END=$(date +%s%N)
        INCR_MS=$(( (INCR_END - INCR_START) / 1000000 ))
        echo "  Chained incremental: ${INCR_MS}ms"
        echo "  Speedup: $(( MONO_REBUILD_MS / (INCR_MS > 0 ? INCR_MS : 1) ))x"

        # Verify the incremental PCH works
        cat > "$WORKDIR/test_incr.cpp" << 'SRCEOF'
int main() {
    auto now = std::chrono::system_clock::now();
    std::vector<std::string> v = {"hello"};
    std::cout << v[0] << std::endl;
    return 0;
}
SRCEOF
        echo "  Verifying incremental PCH..."
        if $CLANG $STD -include-pch "$EXTRA_PCH" -fsyntax-only "$WORKDIR/test_incr.cpp" 2>&1; then
            echo "  Result: PASS"
        else
            echo "  Result: FAIL"
        fi
    else
        echo "  Chained incremental: FAIL (build error)"
    fi
else
    echo "  Skipped (chain build failed earlier)"
fi

echo ""
echo "========================================="
echo "SUMMARY"
echo "========================================="
echo "  Chain length:           $NUM_HEADERS headers"
echo "  Monolithic PCH build:   ${MONO_MS}ms"
echo "  Chained PCH total:      ${CHAIN_TOTAL_MS}ms"
echo "  Monolithic rebuild:     ${MONO_REBUILD_MS}ms"
if [ -n "${INCR_MS:-}" ]; then
echo "  Chained incremental:    ${INCR_MS}ms"
fi
