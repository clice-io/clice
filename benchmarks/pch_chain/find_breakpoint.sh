#!/bin/bash
set -uo pipefail

CLANG="${CLANG:-clang++}"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

HEADERS=(algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat charconv chrono cinttypes climits clocale cmath codecvt compare complex concepts condition_variable coroutine csetjmp csignal cstdarg cstddef cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype)

echo "=== Finding chain depth breakpoint for <deque> ==="

for depth in 1 2 3 5 8 10 12 14 16 18 20 22 24 26 28 30 32 34 36; do
    if [ $depth -gt ${#HEADERS[@]} ]; then
        break
    fi

    PREV=""
    for ((i=0; i<depth; i++)); do
        hdr="${HEADERS[$i]}"
        echo "#include <$hdr>" > "$WORKDIR/l_${i}.h"
        if [ -z "$PREV" ]; then
            $CLANG -x c++-header -std=c++20 -o "$WORKDIR/l_${i}.pch" "$WORKDIR/l_${i}.h" 2>/dev/null
        else
            $CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$WORKDIR/l_${i}.pch" "$WORKDIR/l_${i}.h" 2>/dev/null
        fi
        PREV="$WORKDIR/l_${i}.pch"
    done

    echo "#include <deque>" > "$WORKDIR/test_deque.h"
    ERR=$($CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$WORKDIR/test_deque.pch" "$WORKDIR/test_deque.h" 2>&1)
    RC=$?
    if [ $RC -eq 0 ]; then
        echo "  depth=$depth: PASS"
    else
        ERRMSG=$(echo "$ERR" | grep "error:" | head -1)
        echo "  depth=$depth: FAIL — $ERRMSG"
    fi

    rm -f "$WORKDIR"/l_*.h "$WORKDIR"/l_*.pch "$WORKDIR"/test_deque.*
done
