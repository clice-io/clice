cmake_minimum_required(VERSION 3.30)

# Cross-compilation support via CLICE_TARGET_TRIPLE.
# Examples:
#   -DCLICE_TARGET_TRIPLE=x86_64-apple-darwin      (macOS x64 from arm64)
#   -DCLICE_TARGET_TRIPLE=aarch64-linux-gnu         (Linux arm64 from x64)
#   -DCLICE_TARGET_TRIPLE=aarch64-pc-windows-msvc   (Windows arm64 from x64)
if(DEFINED CLICE_TARGET_TRIPLE)
    if(CLICE_TARGET_TRIPLE MATCHES "^x86_64-apple-darwin")
        set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "")
    elseif(CLICE_TARGET_TRIPLE MATCHES "^aarch64-.*linux")
        set(CMAKE_SYSTEM_NAME Linux)
        set(CMAKE_SYSTEM_PROCESSOR aarch64)
        set(CMAKE_C_COMPILER_TARGET "aarch64-linux-gnu" CACHE STRING "")
        set(CMAKE_CXX_COMPILER_TARGET "aarch64-linux-gnu" CACHE STRING "")
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

if(WIN32)
    find_program(SCCACHE_PATH "sccache")
    if(SCCACHE_PATH)
        set(CMAKE_C_COMPILER_LAUNCHER "${SCCACHE_PATH}" CACHE FILEPATH "")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${SCCACHE_PATH}" CACHE FILEPATH "")
    endif()
    # TODO(prebuilt-respin): switch to "MultiThreaded" (/MT) so the shipped exe
    # is self-contained. /MD makes it import MSVCP140.dll and VCRUNTIME140.dll
    # (x64 also VCRUNTIME140_1.dll), which ship with the Visual C++
    # redistributable rather than with Windows, so a machine without the
    # redistributable cannot start clice. This flag cannot be flipped on its
    # own: every object embeds /DEFAULTLIB and /FAILIFMISMATCH directives
    # naming its CRT, so linking a /MD prebuilt LLVM into a /MT clice is a hard
    # link error, not a warning. The prebuilt is built with this same toolchain
    # file, so it has to be respun to /MT in the same change. Local Windows
    # Debug builds would also need the static ASan runtime in place of the
    # clang_rt.asan_dynamic pair linked in CMakeLists.txt.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld-link")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld-link")
    set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld-link")
else()
    find_program(CCACHE_PATH "ccache")
    if(CCACHE_PATH)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PATH}" CACHE FILEPATH "")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PATH}" CACHE FILEPATH "")
    endif()
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
    set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
endif()

if(APPLE)
    # TODO(prebuilt-respin): set an explicit CMAKE_OSX_DEPLOYMENT_TARGET. With
    # none, the deployment target follows whatever the build machine runs, so
    # the shipped binary is stamped minos 15.0 and refuses to launch on macOS
    # 14 or older. Lowering it needs the prebuilt LLVM built against the same
    # target, and needs availability annotations to stay on (see the kotatsu
    # _LIBCPP_DISABLE_AVAILABILITY removal), so it belongs with the respin.

    # conda-forge clang 22's bundled config files (<triple>-clang++.cfg)
    # inject -L/-rpath pointing into the conda env at link time, binding
    # binaries to conda's @rpath libc++ — they then fail to load outside
    # the build machine. Disable config files: libc++ headers are still
    # found relative to the driver, and links fall back to the SDK's
    # system libc++ (safe thanks to availability annotations).
    string(APPEND CMAKE_C_FLAGS_INIT " --no-default-config")
    string(APPEND CMAKE_CXX_FLAGS_INIT " --no-default-config")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " --no-default-config")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " --no-default-config")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT " --no-default-config")

    # TODO(prebuilt-respin): remove this once the prebuilt is respun — its
    # dylibs will then link the system libc++ via --no-default-config above.
    # Debug links the prebuilt LLVM ASan dylibs, which reference conda's
    # @rpath libc++ with rpaths baked for the machine that built them.
    # Debug binaries are CI-internal, so resolve libc++ from the build
    # env instead.
    if(DEFINED ENV{CONDA_PREFIX})
        string(APPEND CMAKE_EXE_LINKER_FLAGS_DEBUG_INIT " -Wl,-rpath,$ENV{CONDA_PREFIX}/lib")
        string(APPEND CMAKE_SHARED_LINKER_FLAGS_DEBUG_INIT " -Wl,-rpath,$ENV{CONDA_PREFIX}/lib")
    endif()
endif()
