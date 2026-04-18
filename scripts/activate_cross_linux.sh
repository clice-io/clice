#!/bin/sh
# Clear conda cross-gcc flags so host x86_64 paths don't leak into the
# aarch64 build. conda's gcc_linux-aarch64 activation sets
# CFLAGS/CXXFLAGS/CPPFLAGS/LDFLAGS with -isystem/-L pointing at $CONDA_PREFIX
# (x86_64 host paths). LIBRARY_PATH from ld_impl_linux-64 likewise points at
# host libs. Empty-string export reliably overrides conda-installed values
# regardless of whether pixi sources or calls this script.
export CFLAGS=
export CXXFLAGS=
export CPPFLAGS=
export LDFLAGS=
export LIBRARY_PATH=
