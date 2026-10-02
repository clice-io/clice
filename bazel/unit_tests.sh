#!/usr/bin/env bash
# bazel test wrapper: the unit tests from the dist layout, on the checked-in
# fixtures.
set -eu
root=$TEST_SRCDIR/_main
exe=$root/dist/bin/unit_tests
[ -e "$exe" ] || exe=$exe.exe
# Runfiles are symlinks, and project discovery deliberately places a
# symlinked source where it points: the fixtures need real files.
cp -RL "$root/tests/data" "$TEST_TMPDIR/data"
export CLICE_TEST_DATA_DIR=$TEST_TMPDIR/data
# test-setup.sh puts "." first on PATH, which makes every toolchain probe
# cwd-sensitive.
export PATH=${PATH#.:}
# Formatting finds the repository's .clang-format from the working directory.
exec "$exe" "$@"
