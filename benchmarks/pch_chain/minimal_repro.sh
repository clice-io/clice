#!/bin/bash
#
# Minimal reproduction: Chained PCH loses macro defined in c++config.h
#
# Bug: When building a chained PCH with <string>.pch -> <cmath>.pch,
# the __N macro (defined in libstdc++'s bits/c++config.h line 742)
# becomes invisible to subsequent compilation units, even though
# other macros from the same file (e.g. _GLIBCXX_VISIBILITY) survive.
#
# This is NOT a chain-depth issue (50+ trivial chains work fine).
# It is specifically triggered by <cmath> in a chained PCH context.
# Monolithic PCH with both <string> and <cmath> works correctly.
#
set -euo pipefail

CLANG="${CLANG:-clang++}"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "clang: $($CLANG --version | head -1)"
echo ""

cat > "$WORKDIR/test.cpp" << 'EOF'
// Test: use __N macro from libstdc++'s c++config.h
#include <bits/c++config.h>
const char* f() { return __N("test"); }
EOF

# Case 1: Solo <cmath> PCH → PASS
echo "Case 1: solo <cmath> PCH"
echo "#include <cmath>" > "$WORKDIR/solo.h"
$CLANG -x c++-header -std=c++20 -o "$WORKDIR/solo.pch" "$WORKDIR/solo.h" 2>/dev/null
if $CLANG -std=c++20 -include-pch "$WORKDIR/solo.pch" -fsyntax-only "$WORKDIR/test.cpp" 2>/dev/null; then
    echo "  Result: PASS (expected)"
else
    echo "  Result: FAIL (unexpected)"
fi

# Case 2: Monolithic <string>+<cmath> PCH → PASS
echo "Case 2: monolithic <string>+<cmath> PCH"
printf '#include <string>\n#include <cmath>\n' > "$WORKDIR/mono.h"
$CLANG -x c++-header -std=c++20 -o "$WORKDIR/mono.pch" "$WORKDIR/mono.h" 2>/dev/null
if $CLANG -std=c++20 -include-pch "$WORKDIR/mono.pch" -fsyntax-only "$WORKDIR/test.cpp" 2>/dev/null; then
    echo "  Result: PASS (expected)"
else
    echo "  Result: FAIL (unexpected)"
fi

# Case 3: Chained <string>.pch → <cmath>.pch → FAIL
echo "Case 3: chained <string> -> <cmath> PCH"
echo "#include <string>" > "$WORKDIR/link1.h"
$CLANG -x c++-header -std=c++20 -o "$WORKDIR/link1.pch" "$WORKDIR/link1.h" 2>/dev/null
echo "#include <cmath>" > "$WORKDIR/link2.h"
$CLANG -x c++-header -std=c++20 -include-pch "$WORKDIR/link1.pch" -o "$WORKDIR/link2.pch" "$WORKDIR/link2.h" 2>/dev/null
if $CLANG -std=c++20 -include-pch "$WORKDIR/link2.pch" -fsyntax-only "$WORKDIR/test.cpp" 2>&1; then
    echo "  Result: PASS"
else
    echo "  Result: FAIL ← BUG: __N macro lost in chained PCH"
fi

echo ""
echo "Note: _GLIBCXX_VISIBILITY (line 72 of c++config.h) survives."
echo "      __N (line 742 of c++config.h) is lost."
echo "      Both are unconditional #define, no #undef anywhere."
