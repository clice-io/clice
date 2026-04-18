@echo off
REM Clear conda host flags so host x64 paths don't leak into the aarch64-windows
REM cross build. See scripts/activate_cross_linux.sh for rationale.
set "CFLAGS="
set "CXXFLAGS="
set "CPPFLAGS="
set "LDFLAGS="
set "LIBRARY_PATH="
