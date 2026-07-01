include_guard()
include(FetchContent)

function(_download_llvm LLVM_VERSION)
    if(DEFINED CLICE_TARGET_TRIPLE)
        if(CLICE_TARGET_TRIPLE MATCHES "linux")
            set(_PLATFORM "linux")
            set(_TOOLCHAIN "gnu")
        elseif(CLICE_TARGET_TRIPLE MATCHES "darwin")
            set(_PLATFORM "macos")
            set(_TOOLCHAIN "clang")
        elseif(CLICE_TARGET_TRIPLE MATCHES "windows")
            set(_PLATFORM "windows")
            set(_TOOLCHAIN "msvc")
        else()
            message(FATAL_ERROR "Unsupported platform: ${CLICE_TARGET_TRIPLE}")
        endif()

        if(CLICE_TARGET_TRIPLE MATCHES "^aarch64")
            set(_ARCH "arm64")
        elseif(CLICE_TARGET_TRIPLE MATCHES "^x86_64")
            set(_ARCH "x64")
        else()
            message(FATAL_ERROR "Unsupported arch: ${CLICE_TARGET_TRIPLE}")
        endif()
    else()
        if(WIN32)
            set(_PLATFORM "windows")
            set(_TOOLCHAIN "msvc")
        elseif(APPLE)
            set(_PLATFORM "macos")
            set(_TOOLCHAIN "clang")
        else()
            set(_PLATFORM "linux")
            set(_TOOLCHAIN "gnu")
        endif()

        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
            set(_ARCH "arm64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|x64")
            set(_ARCH "x64")
        else()
            message(FATAL_ERROR "Unsupported processor: ${CMAKE_SYSTEM_PROCESSOR}")
        endif()
    endif()

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

    set(_FILENAME "${_ARCH}-${_PLATFORM}-${_TOOLCHAIN}-${_MODE}${_SUFFIX}.tar.xz")
    string(REPLACE "+" "%2B" _URL_VERSION "${LLVM_VERSION}")

    FetchContent_Declare(llvm_prebuilt
        URL "https://github.com/clice-io/clice-llvm/releases/download/${_URL_VERSION}/${_FILENAME}"
        SOURCE_SUBDIR _none
    )
    FetchContent_MakeAvailable(llvm_prebuilt)

    set(LLVM_INSTALL_PATH "${llvm_prebuilt_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(setup_llvm LLVM_VERSION)
    if(DEFINED LLVM_INSTALL_PATH AND NOT LLVM_INSTALL_PATH STREQUAL "")
        get_filename_component(LLVM_INSTALL_PATH "${LLVM_INSTALL_PATH}" ABSOLUTE)
    elseif(DEFINED CLICE_OFFLINE_BUILD AND CLICE_OFFLINE_BUILD)
        message(FATAL_ERROR "LLVM_INSTALL_PATH must be set in offline mode")
    else()
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
        clangFormat clangFrontend clangLex clangSema clangSerialization
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
