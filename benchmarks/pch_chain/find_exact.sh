#!/bin/bash
set -uo pipefail

CLANG="${CLANG:-clang++}"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

HEADERS=(algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat charconv chrono cinttypes climits clocale cmath codecvt compare complex concepts condition_variable coroutine csetjmp csignal cstdarg cstddef cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype)

echo "=== Exact breakpoint: depth 16-18 ==="

for depth in 16 17 18; do
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

    echo "#include <deque>" > "$WORKDIR/test.h"
    ERR=$($CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$WORKDIR/test.pch" "$WORKDIR/test.h" 2>&1)
    RC=$?
    echo "  depth=$depth (last hdr=${HEADERS[$((depth-1))]}): $([ $RC -eq 0 ] && echo PASS || echo FAIL)"

    rm -f "$WORKDIR"/l_*.h "$WORKDIR"/l_*.pch "$WORKDIR"/test.*
done

echo ""
echo "=== Confirming: is it depth or is it the specific header? ==="
echo "depth=17 headers: ${HEADERS[*]:0:17}"
echo ""

# Test: 17 links but with different ordering — is it just depth?
echo "Test: 17 trivial headers (cstddef repeated style)..."
PREV=""
for ((i=0; i<17; i++)); do
    echo "#include <cstddef>" > "$WORKDIR/triv_${i}.h"
    if [ -z "$PREV" ]; then
        $CLANG -x c++-header -std=c++20 -o "$WORKDIR/triv_${i}.pch" "$WORKDIR/triv_${i}.h" 2>/dev/null
    else
        # Each link is a no-op (header guard prevents re-inclusion)
        $CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$WORKDIR/triv_${i}.pch" "$WORKDIR/triv_${i}.h" 2>/dev/null
    fi
    PREV="$WORKDIR/triv_${i}.pch"
done
echo "#include <deque>" > "$WORKDIR/test.h"
ERR=$($CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$WORKDIR/test.pch" "$WORKDIR/test.h" 2>&1)
echo "  17 trivial links + deque: $([ $? -eq 0 ] && echo PASS || echo FAIL)"
rm -f "$WORKDIR"/triv_*.h "$WORKDIR"/triv_*.pch "$WORKDIR"/test.*

# Test: 30 trivial headers
PREV=""
for ((i=0; i<30; i++)); do
    echo "#include <cstddef>" > "$WORKDIR/triv_${i}.h"
    if [ -z "$PREV" ]; then
        $CLANG -x c++-header -std=c++20 -o "$WORKDIR/triv_${i}.pch" "$WORKDIR/triv_${i}.h" 2>/dev/null
    else
        $CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$WORKDIR/triv_${i}.pch" "$WORKDIR/triv_${i}.h" 2>/dev/null
    fi
    PREV="$WORKDIR/triv_${i}.pch"
done
echo "#include <deque>" > "$WORKDIR/test.h"
ERR=$($CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$WORKDIR/test.pch" "$WORKDIR/test.h" 2>&1)
echo "  30 trivial links + deque: $([ $? -eq 0 ] && echo PASS || echo FAIL)"
rm -f "$WORKDIR"/triv_*.h "$WORKDIR"/triv_*.pch "$WORKDIR"/test.*

# Test: 50 trivial headers
PREV=""
for ((i=0; i<50; i++)); do
    echo "#include <cstddef>" > "$WORKDIR/triv_${i}.h"
    if [ -z "$PREV" ]; then
        $CLANG -x c++-header -std=c++20 -o "$WORKDIR/triv_${i}.pch" "$WORKDIR/triv_${i}.h" 2>/dev/null
    else
        $CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$WORKDIR/triv_${i}.pch" "$WORKDIR/triv_${i}.h" 2>/dev/null
    fi
    PREV="$WORKDIR/triv_${i}.pch"
done
echo "#include <deque>" > "$WORKDIR/test.h"
ERR=$($CLANG -x c++-header -std=c++20 -include-pch "$PREV" -o "$WORKDIR/test.pch" "$WORKDIR/test.h" 2>&1)
echo "  50 trivial links + deque: $([ $? -eq 0 ] && echo PASS || echo FAIL)"
