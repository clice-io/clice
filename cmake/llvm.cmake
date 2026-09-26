include_guard()

# Canonical target triple: the explicit CLICE_TARGET_TRIPLE for cross
# builds, composed from the host otherwise. This exact spelling names the
# libclang archives and the clice release assets.
function(clice_target_triple OUT_VAR)
    if(DEFINED CLICE_TARGET_TRIPLE)
        set(${OUT_VAR} "${CLICE_TARGET_TRIPLE}" PARENT_SCOPE)
        return()
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
        set(_ARCH "aarch64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|x64")
        set(_ARCH "x86_64")
    else()
        message(FATAL_ERROR "Unsupported processor: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    if(WIN32)
        set(${OUT_VAR} "${_ARCH}-w64-mingw32" PARENT_SCOPE)
    elseif(APPLE)
        set(${OUT_VAR} "${_ARCH}-apple-darwin" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "${_ARCH}-unknown-linux-gnu" PARENT_SCOPE)
    endif()
endfunction()

# The targets xclang builds an ASan-instrumented libclang for: Debug builds
# for them are ASan builds; elsewhere Debug links the release libclang,
# uninstrumented (libc++'s container annotations would report false
# overflows across the instrumented and uninstrumented halves).
function(clice_asan_available OUT_VAR)
    clice_target_triple(_triple)
    if(_triple MATCHES "^(x86_64-unknown-linux-gnu|aarch64-apple-darwin)$")
        set(${OUT_VAR} ON PARENT_SCOPE)
    else()
        set(${OUT_VAR} OFF PARENT_SCOPE)
    endif()
endfunction()

# libclang-<version>-<triple>[-asan].tar.xz of an xclang release
# (https://github.com/clice-io/xclang): LLVM's and clang's static libraries
# and headers, built by the xclang toolchain of the same version with its
# libc++, and zlib and zstd, which LLVMConfig.cmake looks for.
function(_download_llvm LLVM_VERSION)
    clice_target_triple(_TRIPLE)
    set(_FILENAME "libclang-${LLVM_VERSION}-${_TRIPLE}")
    if(CLICE_ENABLE_ASAN)
        string(APPEND _FILENAME "-asan")
    endif()

    CPMAddPackage(
        NAME llvm_prebuilt
        VERSION ${LLVM_VERSION}
        URL "https://github.com/clice-io/xclang/releases/download/${LLVM_VERSION}/${_FILENAME}.tar.xz"
        DOWNLOAD_ONLY YES
    )

    if(NOT EXISTS "${llvm_prebuilt_SOURCE_DIR}/lib/cmake/llvm")
        # An interrupted download leaves a directory in the cache that CPM
        # would keep treating as the package; a directory the developer
        # pointed at through CPM_llvm_prebuilt_SOURCE is theirs to fix.
        cmake_path(IS_PREFIX CPM_SOURCE_CACHE "${llvm_prebuilt_SOURCE_DIR}" NORMALIZE _in_cache)
        if(_in_cache)
            file(REMOVE_RECURSE "${llvm_prebuilt_SOURCE_DIR}")
            message(FATAL_ERROR
                "The LLVM archive at ${llvm_prebuilt_SOURCE_DIR} was incomplete and has been "
                "removed; run the configure again.")
        endif()
        message(FATAL_ERROR
            "No LLVM install at ${llvm_prebuilt_SOURCE_DIR}: lib/cmake/llvm is missing.")
    endif()

    set(LLVM_INSTALL_PATH "${llvm_prebuilt_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(setup_llvm LLVM_VERSION)
    if(DEFINED LLVM_INSTALL_PATH AND NOT LLVM_INSTALL_PATH STREQUAL "")
        get_filename_component(LLVM_INSTALL_PATH "${LLVM_INSTALL_PATH}" ABSOLUTE)
        if(NOT EXISTS "${LLVM_INSTALL_PATH}/lib/cmake/llvm")
            # The path is cached below, so a wiped source cache would otherwise
            # keep every later configure of this build tree pointed at nothing;
            # a path given by hand is reported instead.
            cmake_path(IS_PREFIX CPM_SOURCE_CACHE "${LLVM_INSTALL_PATH}" NORMALIZE _in_cache)
            if(NOT _in_cache)
                message(FATAL_ERROR
                    "No LLVM install at ${LLVM_INSTALL_PATH}: lib/cmake/llvm is missing. Point "
                    "LLVM_INSTALL_PATH at an LLVM install, or unset it (-ULLVM_INSTALL_PATH) to "
                    "download the prebuilt one.")
            endif()
            message(STATUS "LLVM not found at ${LLVM_INSTALL_PATH}, downloading")
            unset(LLVM_INSTALL_PATH)
            unset(LLVM_INSTALL_PATH CACHE)
        endif()
    endif()

    if(NOT DEFINED LLVM_INSTALL_PATH OR LLVM_INSTALL_PATH STREQUAL "")
        if(CLICE_OFFLINE_BUILD)
            message(FATAL_ERROR "LLVM_INSTALL_PATH must be set in offline mode")
        endif()
        _download_llvm("${LLVM_VERSION}")
    endif()

    set(LLVM_INSTALL_PATH "${LLVM_INSTALL_PATH}" CACHE PATH "LLVM install" FORCE)

    _check_llvm_manifest("${LLVM_INSTALL_PATH}")

    # LLVMConfig.cmake finds the archive's zlib and zstd through the prefix
    # path.
    list(PREPEND CMAKE_PREFIX_PATH "${LLVM_INSTALL_PATH}")
    find_package(LLVM REQUIRED CONFIG
        PATHS "${LLVM_INSTALL_PATH}/lib/cmake/llvm" NO_DEFAULT_PATH)
    find_package(Clang REQUIRED CONFIG
        PATHS "${LLVM_INSTALL_PATH}/lib/cmake/clang" NO_DEFAULT_PATH)

    llvm_map_components_to_libnames(LLVM_RESOLVED
        support frontendopenmp option targetparser)

    add_library(llvm-libs INTERFACE IMPORTED)
    target_link_libraries(llvm-libs INTERFACE
        ${LLVM_RESOLVED}
        clangAST clangASTMatchers clangBasic clangDriver
        clangFormat clangFrontend clangLex clangOptions clangSema clangSerialization
        clangToolingInclusionsStdlib
        clangTidy clangTidyUtils
        clangTidyAbseilModule clangTidyAlteraModule clangTidyAndroidModule
        clangTidyBoostModule clangTidyBugproneModule clangTidyCERTModule
        clangTidyConcurrencyModule clangTidyCppCoreGuidelinesModule
        clangTidyDarwinModule clangTidyFuchsiaModule
        clangTidyGoogleModule clangTidyLinuxKernelModule
        clangTidyLLVMModule clangTidyLLVMLibcModule clangTidyMiscModule
        clangTidyModernizeModule clangTidyObjCModule
        clangTidyOpenMPModule clangTidyPerformanceModule
        clangTidyPortabilityModule clangTidyReadabilityModule
        clangTidyZirconModule
        clangTooling clangToolingCore
        clangToolingInclusions clangToolingInclusionsStdlib clangToolingSyntax
    )

    target_include_directories(llvm-libs SYSTEM INTERFACE
        "${LLVM_INSTALL_PATH}/include")
    target_compile_definitions(llvm-libs INTERFACE CLANG_BUILD_STATIC=1)

    message(STATUS "LLVM ${LLVM_VERSION} at ${LLVM_INSTALL_PATH}")
endfunction()

# The archive records how it was built. Every field checked here is one
# where a mismatch still links and then fails at runtime, or fails the link
# with an error that does not name the cause.
function(_check_llvm_manifest install_path)
    set(_manifest "${install_path}/lib/cmake/xclang/libclang.cmake")
    if(NOT EXISTS "${_manifest}")
        message(FATAL_ERROR
            "No xclang manifest at ${_manifest}: ${install_path} is not an xclang libclang. "
            "Point LLVM_INSTALL_PATH at one, or unset it (-ULLVM_INSTALL_PATH) to download it.")
    endif()
    include("${_manifest}")
    clice_target_triple(_triple)
    if(CLICE_ENABLE_ASAN)
        set(_expected_asan ON)
    else()
        set(_expected_asan OFF)
    endif()

    set(_mismatch "")
    if(NOT XCLANG_TARGET_TRIPLE STREQUAL _triple)
        string(APPEND _mismatch "\n  target: package ${XCLANG_TARGET_TRIPLE}, this build ${_triple}")
    endif()
    # The release archives hold ThinLTO bitcode, which only the LLVM that
    # wrote it is sure to read.
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
            OR NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL XCLANG_LLVM_VERSION)
        string(APPEND _mismatch "\n  compiler: package xclang ${XCLANG_LLVM_VERSION}, this build "
            "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
    if(NOT XCLANG_ASAN STREQUAL _expected_asan)
        string(APPEND _mismatch "\n  ASan: package ${XCLANG_ASAN}, this build ${_expected_asan}")
    endif()
    if(_mismatch)
        message(FATAL_ERROR "The LLVM package at ${install_path} does not match this build:${_mismatch}")
    endif()
endfunction()
