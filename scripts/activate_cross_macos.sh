#!/bin/sh
# Clear conda host flags so arm64 host paths don't leak into the x86_64-macos
# cross build. See scripts/activate_cross_linux.sh for rationale.
export CFLAGS=
export CXXFLAGS=
export CPPFLAGS=
export LDFLAGS=
export LIBRARY_PATH=
