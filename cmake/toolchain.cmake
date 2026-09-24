cmake_minimum_required(VERSION 3.30)

# Windows through mingw-w64, from any host: a `<arch>-w64-mingw32` target
# triple selects the mingw-w64 sysroot and libgcc that conda-forge ships
# (m2w64-sysroot_win-64 and libgcc-devel_win-64, pulled in by the pixi
# feature); clang and lld are the pixi ones. libgcc serves as builtins and
# unwinder like on Linux. With CLICE_MINGW_ROOT set in the environment the
# sysroot, libunwind and compiler-rt come from that llvm-mingw instead (the
# only source for aarch64).
if(DEFINED CLICE_TARGET_TRIPLE AND CLICE_TARGET_TRIPLE MATCHES "^([a-z0-9_]+)-w64-mingw32$")
    set(_mingw_arch "${CMAKE_MATCH_1}")
    if(NOT CMAKE_HOST_WIN32)
        set(CMAKE_SYSTEM_NAME Windows)
        if(_mingw_arch STREQUAL "aarch64")
            set(CMAKE_SYSTEM_PROCESSOR ARM64)
        else()
            set(CMAKE_SYSTEM_PROCESSOR AMD64)
        endif()
    endif()
    set(CMAKE_C_COMPILER_TARGET "${CLICE_TARGET_TRIPLE}" CACHE STRING "")
    set(CMAKE_CXX_COMPILER_TARGET "${CLICE_TARGET_TRIPLE}" CACHE STRING "")
    # As compiler arguments rather than *_FLAGS_INIT: a -DCMAKE_<LANG>_FLAGS
    # on the command line (scripts/build-llvm.py gives one) replaces the
    # initial flags, and the driver only finds this libgcc through -L.
    # -nostdlib++ keeps CMake's compiler check from asking for a libstdc++
    # the sysroot does not have; the C++ standard library is the package's
    # libc++, named by cmake/llvm.cmake. The link-only arguments would be
    # "unused" on every compile, an error under a dependency's -Werror.
    if(DEFINED ENV{CLICE_MINGW_ROOT})
        # An llvm-mingw installation instead (conda-forge has no aarch64
        # sysroot, and GCC no aarch64 mingw target): its per-target sysroot
        # with libunwind, compiler-rt builtins in place of libgcc. The pixi
        # clang looks for the builtins in its own resource directory, where
        # CI copies llvm-mingw's.
        file(TO_CMAKE_PATH "$ENV{CLICE_MINGW_ROOT}" _mingw_root)
        set(CMAKE_SYSROOT "${_mingw_root}/${CLICE_TARGET_TRIPLE}")
        if(NOT EXISTS "${CMAKE_SYSROOT}/include/windows.h")
            message(FATAL_ERROR "No ${CLICE_TARGET_TRIPLE} sysroot under CLICE_MINGW_ROOT=${_mingw_root}")
        endif()
        set(_mingw_args "-rtlib=compiler-rt;-unwindlib=libunwind;-nostdlib++;-Qunused-arguments")
    else()
        if(NOT DEFINED ENV{CONDA_PREFIX})
            message(FATAL_ERROR "A mingw target needs the pixi environment for its sysroot (CONDA_PREFIX is unset)")
        endif()
        if(CMAKE_HOST_WIN32)
            set(_conda_lib "$ENV{CONDA_PREFIX}/Library")
        else()
            set(_conda_lib "$ENV{CONDA_PREFIX}")
        endif()
        file(TO_CMAKE_PATH "${_conda_lib}" _conda_lib)
        set(CMAKE_SYSROOT "${_conda_lib}/${CLICE_TARGET_TRIPLE}/sysroot/usr")
        file(GLOB _mingw_gcc_dir LIST_DIRECTORIES true "${_conda_lib}/lib/gcc/${CLICE_TARGET_TRIPLE}/*")
        if(NOT EXISTS "${CMAKE_SYSROOT}/include/windows.h" OR NOT _mingw_gcc_dir)
            message(FATAL_ERROR
                "No mingw-w64 sysroot or libgcc under ${_conda_lib}: the pixi environment lacks "
                "m2w64-sysroot_win-64 / libgcc-devel_win-64 (feature cross-windows-mingw); "
                "set CLICE_MINGW_ROOT to an llvm-mingw installation instead")
        endif()
        list(GET _mingw_gcc_dir 0 _mingw_gcc_dir)
        set(_mingw_args "-rtlib=libgcc;-unwindlib=libgcc;-nostdlib++;-L${_mingw_gcc_dir};-Qunused-arguments")
    endif()
    set(CMAKE_C_COMPILER "clang;${_mingw_args}" CACHE STRING "")
    set(CMAKE_CXX_COMPILER "clang++;${_mingw_args}" CACHE STRING "")
    find_program(LLVM_WINDRES_PATH "llvm-windres")
    set(CMAKE_RC_COMPILER "${LLVM_WINDRES_PATH}" CACHE FILEPATH "")
    set(CMAKE_RC_FLAGS "--target=${CLICE_TARGET_TRIPLE}" CACHE STRING "")
endif()

# Cross-compilation support via CLICE_TARGET_TRIPLE.
# Examples:
#   -DCLICE_TARGET_TRIPLE=x86_64-apple-darwin       (macOS x64 from arm64)
#   -DCLICE_TARGET_TRIPLE=aarch64-unknown-linux-gnu (Linux arm64 from x64)
#   -DCLICE_TARGET_TRIPLE=aarch64-pc-windows-msvc   (Windows arm64 from x64)
if(DEFINED CLICE_TARGET_TRIPLE)
    if(CLICE_TARGET_TRIPLE MATCHES "^x86_64-apple-darwin")
        set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "")
    elseif(CLICE_TARGET_TRIPLE MATCHES "^aarch64-.*linux")
        set(CMAKE_SYSTEM_NAME Linux)
        set(CMAKE_SYSTEM_PROCESSOR aarch64)
        set(CMAKE_C_COMPILER_TARGET "aarch64-unknown-linux-gnu" CACHE STRING "")
        set(CMAKE_CXX_COMPILER_TARGET "aarch64-unknown-linux-gnu" CACHE STRING "")
        if(DEFINED ENV{CONDA_PREFIX} AND NOT DEFINED CMAKE_SYSROOT)
            set(CMAKE_SYSROOT "$ENV{CONDA_PREFIX}/aarch64-conda-linux-gnu/sysroot" CACHE PATH "")
        endif()
    elseif(CLICE_TARGET_TRIPLE MATCHES "^aarch64-.*-windows")
        set(CMAKE_SYSTEM_NAME Windows)
        set(CMAKE_SYSTEM_PROCESSOR ARM64)
        set(CMAKE_C_COMPILER_TARGET "aarch64-pc-windows-msvc" CACHE STRING "")
        set(CMAKE_CXX_COMPILER_TARGET "aarch64-pc-windows-msvc" CACHE STRING "")
    endif()
endif()

set(CMAKE_C_COMPILER clang CACHE STRING "")
set(CMAKE_CXX_COMPILER clang++ CACHE STRING "")

find_program(LLVM_AR_PATH "llvm-ar")
if(LLVM_AR_PATH)
    set(CMAKE_AR "${LLVM_AR_PATH}" CACHE FILEPATH "")
    set(CMAKE_C_COMPILER_AR "${LLVM_AR_PATH}" CACHE FILEPATH "")
    set(CMAKE_CXX_COMPILER_AR "${LLVM_AR_PATH}" CACHE FILEPATH "")
endif()

find_program(LLVM_RANLIB_PATH "llvm-ranlib")
if(LLVM_RANLIB_PATH)
    set(CMAKE_RANLIB "${LLVM_RANLIB_PATH}" CACHE FILEPATH "")
    set(CMAKE_C_COMPILER_RANLIB "${LLVM_RANLIB_PATH}" CACHE FILEPATH "")
    set(CMAKE_CXX_COMPILER_RANLIB "${LLVM_RANLIB_PATH}" CACHE FILEPATH "")
endif()

# On macOS, CMake's Ninja generator uses libtool instead of ar for static
# libraries. Apple's libtool cannot read bitcode from newer LLVM versions
# (e.g. attribute kind 102 from LLVM 22), breaking LTO builds. Use LLVM's
# llvm-libtool-darwin if available; otherwise suppress CMAKE_LIBTOOL so
# CMake falls back to CMAKE_AR (llvm-ar handles bitcode correctly).
if(APPLE)
    find_program(LLVM_LIBTOOL_PATH "llvm-libtool-darwin")
    if(LLVM_LIBTOOL_PATH)
        set(CMAKE_LIBTOOL "${LLVM_LIBTOOL_PATH}" CACHE FILEPATH "")
    else()
        set(CMAKE_LIBTOOL "CMAKE_LIBTOOL-NOTFOUND" CACHE FILEPATH "")
    endif()
endif()

find_program(LLVM_NM_PATH "llvm-nm")
if(LLVM_NM_PATH)
    set(CMAKE_NM "${LLVM_NM_PATH}" CACHE FILEPATH "")
endif()

find_program(LLVM_RC_PATH "llvm-rc")
if(LLVM_RC_PATH)
    set(CMAKE_RC_COMPILER "${LLVM_RC_PATH}" CACHE FILEPATH "")
endif()

# set(CACHE) below never replaces an existing entry, so a build tree
# configured before the switch to ccache would keep sccache (or a path that
# no longer exists) forever; drop such entries so the lookup runs again. A
# launcher given as a command name (distcc) is left alone.
foreach(lang C CXX)
    foreach(word IN LISTS CMAKE_${lang}_COMPILER_LAUNCHER)
        if(word MATCHES "(^|/)sccache(\\.exe)?$" OR (IS_ABSOLUTE "${word}" AND NOT EXISTS "${word}"))
            unset(CMAKE_${lang}_COMPILER_LAUNCHER CACHE)
            break()
        endif()
    endforeach()
endforeach()

# ccache treats a precompiled header as uncacheable unless it may ignore the
# defines and time macros baked into it; the launcher carries that setting
# so no per-machine ccache configuration is needed. A bare ccache launcher —
# from an earlier configure or given by hand — is wrapped the same way.
find_program(CCACHE_PATH "ccache")
foreach(lang C CXX)
    set(launcher "${CMAKE_${lang}_COMPILER_LAUNCHER}")
    if(launcher MATCHES "^[^;]*ccache[^;]*$")
        set(ccache "${launcher}")
    elseif(launcher STREQUAL "" AND CCACHE_PATH)
        set(ccache "${CCACHE_PATH}")
    else()
        continue()
    endif()
    set(CMAKE_${lang}_COMPILER_LAUNCHER
        "${CMAKE_COMMAND};-E;env;CCACHE_SLOPPINESS=pch_defines,time_macros;${ccache}"
        CACHE STRING "" FORCE)
endforeach()

if(WIN32 AND NOT CLICE_TARGET_TRIPLE MATCHES "-w64-mingw32$")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld-link")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld-link")
    set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld-link")
else()
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
    set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
endif()

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "")

    # conda-forge clang's bundled config files (<triple>-clang++.cfg)
    # inject -L/-rpath pointing into the conda env at link time, binding
    # binaries to conda's @rpath libc++ — they then fail to load outside
    # the build machine. Disable config files; the standard library is the
    # LLVM package's libc++ (cmake/llvm.cmake).
    string(APPEND CMAKE_C_FLAGS_INIT " --no-default-config")
    string(APPEND CMAKE_CXX_FLAGS_INIT " --no-default-config")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " --no-default-config")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " --no-default-config")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT " --no-default-config")
endif()
