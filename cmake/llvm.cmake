include_guard()

# Canonical target triple: the explicit CLICE_TARGET_TRIPLE for cross
# builds, composed from the host otherwise. This exact spelling names the
# prebuilt LLVM archives and the clice release assets.
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
        set(${OUT_VAR} "${_ARCH}-pc-windows-msvc" PARENT_SCOPE)
    elseif(APPLE)
        set(${OUT_VAR} "${_ARCH}-apple-darwin" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "${_ARCH}-unknown-linux-gnu" PARENT_SCOPE)
    endif()
endfunction()

function(_download_llvm LLVM_VERSION)
    clice_target_triple(_TRIPLE)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_MODE "debug")
    else()
        set(_MODE "releasedbg")
    endif()

    set(_SUFFIX "")
    if(CLICE_ENABLE_LTO)
        string(APPEND _SUFFIX "-lto")
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT WIN32)
        string(APPEND _SUFFIX "-asan")
    endif()

    set(_FILENAME "${_TRIPLE}.${_MODE}${_SUFFIX}.tar.xz")
    string(REPLACE "+" "%2B" _URL_VERSION "${LLVM_VERSION}")
    # A release like 22.1.8+1 is not a CMake version; CPM hands VERSION to
    # find_package when local packages are enabled.
    string(REPLACE "+" "." _CMAKE_VERSION "${LLVM_VERSION}")

    CPMAddPackage(
        NAME llvm_prebuilt
        VERSION ${_CMAKE_VERSION}
        URL "https://github.com/clice-io/clice-llvm/releases/download/${_URL_VERSION}/${_FILENAME}"
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
            # keep every later configure of this build tree pointed at nothing.
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
        clangTidy clangTidyUtils
        clangTidyAbseilModule clangTidyAlteraModule clangTidyAndroidModule
        clangTidyBoostModule clangTidyBugproneModule clangTidyCERTModule
        clangTidyConcurrencyModule clangTidyCppCoreGuidelinesModule
        clangTidyDarwinModule clangTidyFuchsiaModule
        clangTidyGoogleModule clangTidyHICPPModule clangTidyLinuxKernelModule
        clangTidyLLVMModule clangTidyLLVMLibcModule clangTidyMiscModule
        clangTidyModernizeModule clangTidyMPIModule clangTidyObjCModule
        clangTidyOpenMPModule clangTidyPerformanceModule
        clangTidyPortabilityModule clangTidyReadabilityModule
        clangTidyZirconModule
        clangTooling clangToolingCore
        clangToolingInclusions clangToolingInclusionsStdlib clangToolingSyntax
    )

    target_include_directories(llvm-libs SYSTEM INTERFACE
        "${LLVM_INSTALL_PATH}/include")

    if(NOT BUILD_SHARED_LIBS)
        target_compile_definitions(llvm-libs INTERFACE CLANG_BUILD_STATIC=1)
    endif()

    message(STATUS "LLVM ${LLVM_VERSION} at ${LLVM_INSTALL_PATH}")
endfunction()
