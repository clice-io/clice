#!/usr/bin/env bash
# bazel test wrapper: the unit tests from the dist layout, on the checked-in
# fixtures.
set -eu
root=$TEST_SRCDIR/_main
exe=$root/dist/bin/unit_tests
[ -e "$exe" ] || exe=$exe.exe
export CLICE_TEST_DATA_DIR=$root/tests/data
exec "$exe" "$@"
