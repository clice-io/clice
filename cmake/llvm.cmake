include_guard()
include(FetchContent)

function(_download_llvm LLVM_VERSION)
    if(DEFINED CLICE_TARGET_TRIPLE)
        if(CLICE_TARGET_TRIPLE MATCHES "linux")
            set(_PLATFORM "linux")
        elseif(CLICE_TARGET_TRIPLE MATCHES "darwin")
            set(_PLATFORM "macos")
        elseif(CLICE_TARGET_TRIPLE MATCHES "windows")
            set(_PLATFORM "windows")
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
        elseif(APPLE)
            set(_PLATFORM "macos")
        else()
            set(_PLATFORM "linux")
        endif()

        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
            set(_ARCH "arm64")
        else()
            set(_ARCH "x64")
        endif()
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_BUILD_TYPE "Debug")
        if(NOT WIN32)
            set(_ASAN ON)
        else()
            set(_ASAN OFF)
        endif()
    else()
        set(_BUILD_TYPE "RelWithDebInfo")
        set(_ASAN OFF)
    endif()

    if(CLICE_ENABLE_LTO)
        set(_LTO ON)
    else()
        set(_LTO OFF)
    endif()

    file(READ "${PROJECT_SOURCE_DIR}/config/llvm-manifest.json" _JSON)
    string(JSON _COUNT LENGTH "${_JSON}")
    math(EXPR _LAST "${_COUNT} - 1")

    set(_FOUND FALSE)
    foreach(_I RANGE 0 ${_LAST})
        string(JSON _E GET "${_JSON}" ${_I})
        string(JSON _P GET "${_E}" platform)
        string(JSON _A GET "${_E}" arch)
        string(JSON _B GET "${_E}" build_type)
        string(JSON _L GET "${_E}" lto)
        string(JSON _S GET "${_E}" asan)
        if(_P STREQUAL "${_PLATFORM}" AND _A STREQUAL "${_ARCH}"
           AND _B STREQUAL "${_BUILD_TYPE}" AND _L STREQUAL "${_LTO}"
           AND _S STREQUAL "${_ASAN}")
            string(JSON _FILENAME GET "${_E}" filename)
            string(JSON _SHA256 GET "${_E}" sha256)
            set(_FOUND TRUE)
            break()
        endif()
    endforeach()

    if(NOT _FOUND)
        message(FATAL_ERROR
            "No LLVM artifact for: "
            "platform=${_PLATFORM} arch=${_ARCH} build=${_BUILD_TYPE} "
            "lto=${_LTO} asan=${_ASAN}")
    endif()

    string(REPLACE "+" "%2B" _URL_VERSION "${LLVM_VERSION}")
    FetchContent_Declare(llvm_prebuilt
        URL "https://github.com/clice-io/clice-llvm/releases/download/${_URL_VERSION}/${_FILENAME}"
        URL_HASH SHA256=${_SHA256}
        SOURCE_SUBDIR _none
    )
    FetchContent_MakeAvailable(llvm_prebuilt)

    if(EXISTS "${llvm_prebuilt_SOURCE_DIR}/build-install")
        set(LLVM_INSTALL_PATH "${llvm_prebuilt_SOURCE_DIR}/build-install" PARENT_SCOPE)
    else()
        set(LLVM_INSTALL_PATH "${llvm_prebuilt_SOURCE_DIR}" PARENT_SCOPE)
    endif()
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
