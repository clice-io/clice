#!/bin/sh
# Clear LIBRARY_PATH to prevent host x86_64 libraries from being found
# during aarch64 cross-compilation. Conda's ld_impl_linux-64 sets
# LIBRARY_PATH=$CONDA_PREFIX/lib which contains x86_64 libgcc_s.so.1.
unset LIBRARY_PATH
