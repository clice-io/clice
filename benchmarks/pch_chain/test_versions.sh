#!/bin/bash
#
# Test the chained PCH macro loss bug across clang versions.
# Usage: Pass the clang++ binary path as argument.
#
set -euo pipefail

CLANG="$1"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "clang: $($CLANG --version | head -1)"

cat > "$WORKDIR/test.cpp" << 'EOF'
#include <bits/c++config.h>
const char* f() { return __N("test"); }
EOF

echo "#include <string>" > "$WORKDIR/link1.h"
$CLANG -std=c++20 -Xclang -emit-pch -o "$WORKDIR/link1.pch" "$WORKDIR/link1.h" 2>/dev/null

echo "#include <cmath>" > "$WORKDIR/link2.h"
$CLANG -std=c++20 -include-pch "$WORKDIR/link1.pch" -Xclang -emit-pch -o "$WORKDIR/link2.pch" "$WORKDIR/link2.h" 2>/dev/null

if $CLANG -std=c++20 -include-pch "$WORKDIR/link2.pch" -fsyntax-only "$WORKDIR/test.cpp" 2>/dev/null; then
    echo "Result: PASS (bug is fixed)"
else
    echo "Result: FAIL (bug still present)"
fi
