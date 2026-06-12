# ASAN-instrumented binaries reference libclang_rt.asan_osx_dynamic.dylib
# via @rpath entries that point into the environment they were built in.
# Expose the activated env's copy through DYLD_LIBRARY_PATH so the same
# binaries run in other envs (e.g. artifact-based CI test jobs).
CLANG_RES="$(clang++ --print-resource-dir 2>/dev/null)"
if [ -n "$CLANG_RES" ] && [ -d "$CLANG_RES/lib/darwin" ]; then
    export DYLD_LIBRARY_PATH="$CLANG_RES/lib/darwin${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
fi
