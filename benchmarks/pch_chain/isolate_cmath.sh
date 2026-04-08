#!/bin/bash
set -uo pipefail

CLANG="${CLANG:-clang++}"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# We know: 17-link chain ending at clocale + deque = PASS
#          18-link chain ending at cmath + deque = FAIL
# But cmath -> deque alone = PASS
# So it's the combination of deep chain + cmath that causes __N loss

# Test: just the "heavy" headers in chain, then cmath, then deque
echo "=== Test: minimal chain that breaks ==="

# Which headers before cmath are "heavy" (pull in lots of stuff)?
# algorithm, atomic, barrier, charconv, chrono are the big ones
COMBOS=(
    "algorithm cmath"
    "chrono cmath"
    "barrier cmath"
    "algorithm chrono cmath"
    "algorithm atomic barrier chrono cmath"
    "algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat charconv chrono cinttypes climits clocale cmath"
)

for combo_str in "${COMBOS[@]}"; do
    read -ra combo <<< "$combo_str"
    PREV=""
    for hdr in "${combo[@]}"; do
        F="$WORKDIR/t_${hdr}.h"
        P="$WORKDIR/t_${hdr}.pch"
        echo "#include <$hdr>" > "$F"
        if [ -z "$PREV" ]; then
            $CLANG -x c++-header -std=c++20 -o "$P" "$F" 2>/dev/null
        else
            $CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$P" "$F" 2>/dev/null
        fi
        PREV="$P"
    done

    echo "#include <deque>" > "$WORKDIR/test.h"
    ERR=$($CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$WORKDIR/test.pch" "$WORKDIR/test.h" 2>&1)
    RC=$?
    echo "  [${combo_str}] -> deque: $([ $RC -eq 0 ] && echo PASS || echo FAIL) (${#combo[@]} links)"

    rm -f "$WORKDIR"/t_*.h "$WORKDIR"/t_*.pch "$WORKDIR"/test.*
done

echo ""
echo "=== Stripping headers one by one from the 18-header chain ==="
FULL=(algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat charconv chrono cinttypes climits clocale cmath)

for skip in "${FULL[@]}"; do
    CHAIN=()
    for h in "${FULL[@]}"; do
        if [ "$h" != "$skip" ]; then
            CHAIN+=("$h")
        fi
    done

    PREV=""
    for hdr in "${CHAIN[@]}"; do
        F="$WORKDIR/s_${hdr}.h"
        P="$WORKDIR/s_${hdr}.pch"
        echo "#include <$hdr>" > "$F"
        if [ -z "$PREV" ]; then
            $CLANG -x c++-header -std=c++20 -o "$P" "$F" 2>/dev/null
        else
            $CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$P" "$F" 2>/dev/null
        fi
        PREV="$P"
    done

    echo "#include <deque>" > "$WORKDIR/test.h"
    ERR=$($CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$WORKDIR/test.pch" "$WORKDIR/test.h" 2>&1)
    RC=$?
    echo "  skip <$skip> (${#CHAIN[@]} links): $([ $RC -eq 0 ] && echo PASS || echo FAIL)"

    rm -f "$WORKDIR"/s_*.h "$WORKDIR"/s_*.pch "$WORKDIR"/test.*
done
