include_guard()

function(_detect_llvm_artifact_name OUT_FILENAME)
    if(DEFINED CLICE_TARGET_TRIPLE)
        if(CLICE_TARGET_TRIPLE MATCHES "^aarch64")
            set(_ARCH "aarch64")
        elseif(CLICE_TARGET_TRIPLE MATCHES "^x86_64")
            set(_ARCH "x64")
        else()
            message(FATAL_ERROR "Unsupported arch in CLICE_TARGET_TRIPLE: ${CLICE_TARGET_TRIPLE}")
        endif()

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
            message(FATAL_ERROR "Unsupported platform in CLICE_TARGET_TRIPLE: ${CLICE_TARGET_TRIPLE}")
        endif()
    else()
        if(WIN32)
            set(_PLATFORM "windows")
            set(_TOOLCHAIN "msvc")
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
                set(_ARCH "aarch64")
            else()
                set(_ARCH "x64")
            endif()
        elseif(APPLE)
            set(_PLATFORM "macos")
            set(_TOOLCHAIN "clang")
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
                set(_ARCH "arm64")
            else()
                set(_ARCH "x64")
            endif()
        else()
            set(_PLATFORM "linux")
            set(_TOOLCHAIN "gnu")
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
                set(_ARCH "aarch64")
            else()
                set(_ARCH "x64")
            endif()
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

    set(${OUT_FILENAME} "${_ARCH}-${_PLATFORM}-${_TOOLCHAIN}-${_MODE}${_SUFFIX}.tar.xz" PARENT_SCOPE)
endfunction()

function(_download_and_extract _URL _SHA256 _DEST _LABEL)
    set(_DOWNLOAD_PATH "${CMAKE_CURRENT_BINARY_DIR}/${_LABEL}")
    if(EXISTS "${_DOWNLOAD_PATH}")
        file(SHA256 "${_DOWNLOAD_PATH}" _EXISTING_HASH)
        if(NOT _EXISTING_HASH STREQUAL "${_SHA256}")
            message(STATUS "Hash mismatch for cached ${_LABEL}, re-downloading")
            file(REMOVE "${_DOWNLOAD_PATH}")
        endif()
    endif()

    if(NOT EXISTS "${_DOWNLOAD_PATH}")
        message(STATUS "Downloading ${_LABEL}")
        file(DOWNLOAD "${_URL}" "${_DOWNLOAD_PATH}"
            EXPECTED_HASH SHA256=${_SHA256}
            SHOW_PROGRESS
            STATUS _DL_STATUS)
        list(GET _DL_STATUS 0 _DL_CODE)
        if(NOT _DL_CODE EQUAL 0)
            list(GET _DL_STATUS 1 _DL_MSG)
            file(REMOVE "${_DOWNLOAD_PATH}")
            message(FATAL_ERROR "Failed to download ${_LABEL}: ${_DL_MSG}")
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xf "${_DOWNLOAD_PATH}"
        WORKING_DIRECTORY "${_DEST}"
        RESULT_VARIABLE _TAR_RESULT)
    if(NOT _TAR_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to extract ${_LABEL}")
    endif()
endfunction()

function(_download_llvm LLVM_VERSION)
    _detect_llvm_artifact_name(_FILENAME)

    set(_MANIFEST_PATH "${PROJECT_SOURCE_DIR}/config/llvm-manifest.json")
    file(READ "${_MANIFEST_PATH}" _MANIFEST_JSON)

    string(JSON _SHA256 ERROR_VARIABLE _ERR
        GET "${_MANIFEST_JSON}" artifacts "${_FILENAME}" sha256)
    if(_ERR)
        message(FATAL_ERROR "No matching LLVM artifact in manifest for: ${_FILENAME}")
    endif()

    set(_BASE_URL "https://github.com/clice-io/clice-llvm/releases/download/${LLVM_VERSION}")
    set(_INSTALL_ROOT "${CMAKE_CURRENT_BINARY_DIR}/.llvm")
    set(_VERSION_STAMP "${_INSTALL_ROOT}/.llvm-version")

    set(_NEED_INSTALL TRUE)
    if(EXISTS "${_INSTALL_ROOT}/lib/cmake/llvm/LLVMConfig.cmake" AND EXISTS "${_VERSION_STAMP}")
        file(READ "${_VERSION_STAMP}" _CACHED_VERSION)
        string(STRIP "${_CACHED_VERSION}" _CACHED_VERSION)
        if(_CACHED_VERSION STREQUAL "${LLVM_VERSION}")
            set(_NEED_INSTALL FALSE)
        else()
            message(STATUS "LLVM version changed (${_CACHED_VERSION} -> ${LLVM_VERSION}), reinstalling")
            file(REMOVE_RECURSE "${_INSTALL_ROOT}")
        endif()
    endif()

    if(_NEED_INSTALL)
        file(MAKE_DIRECTORY "${_INSTALL_ROOT}")

        _download_and_extract(
            "${_BASE_URL}/${_FILENAME}" "${_SHA256}" "${_INSTALL_ROOT}" "${_FILENAME}")

        if(EXISTS "${_INSTALL_ROOT}/build-install")
            file(GLOB _NESTED "${_INSTALL_ROOT}/build-install/*")
            foreach(_ENTRY ${_NESTED})
                get_filename_component(_NAME "${_ENTRY}" NAME)
                file(RENAME "${_ENTRY}" "${_INSTALL_ROOT}/${_NAME}")
            endforeach()
            file(REMOVE_RECURSE "${_INSTALL_ROOT}/build-install")
        endif()

        file(WRITE "${_VERSION_STAMP}" "${LLVM_VERSION}\n")
    endif()

    set(LLVM_INSTALL_PATH "${_INSTALL_ROOT}" PARENT_SCOPE)
endfunction()

function(setup_llvm LLVM_VERSION)
    if(DEFINED LLVM_INSTALL_PATH AND NOT LLVM_INSTALL_PATH STREQUAL "")
        get_filename_component(LLVM_INSTALL_PATH "${LLVM_INSTALL_PATH}" ABSOLUTE)
        if(NOT EXISTS "${LLVM_INSTALL_PATH}")
            message(FATAL_ERROR "LLVM_INSTALL_PATH does not exist: ${LLVM_INSTALL_PATH}")
        endif()
    elseif(DEFINED CLICE_OFFLINE_BUILD AND CLICE_OFFLINE_BUILD)
        message(FATAL_ERROR "LLVM_INSTALL_PATH must be set in offline mode")
    else()
        _download_llvm("${LLVM_VERSION}")
    endif()

    set(LLVM_INSTALL_PATH "${LLVM_INSTALL_PATH}" CACHE PATH "Path to LLVM installation" FORCE)

    find_package(LLVM REQUIRED CONFIG
        PATHS "${LLVM_INSTALL_PATH}/lib/cmake/llvm" NO_DEFAULT_PATH)
    find_package(Clang REQUIRED CONFIG
        PATHS "${LLVM_INSTALL_PATH}/lib/cmake/clang" NO_DEFAULT_PATH)

    llvm_map_components_to_libnames(LLVM_RESOLVED
        support frontendopenmp option targetparser)

    add_library(llvm-libs INTERFACE IMPORTED)
    target_link_libraries(llvm-libs INTERFACE
        ${LLVM_RESOLVED}
        clangAST clangASTMatchers clangBasic clangDriver clangOptions
        clangFormat clangFrontend clangLex clangSema clangSerialization
        clangTidy clangTidyUtils
        clangTidyAbseilModule clangTidyAlteraModule clangTidyAndroidModule
        clangTidyBoostModule clangTidyBugproneModule clangTidyCERTModule
        clangTidyConcurrencyModule clangTidyCppCoreGuidelinesModule
        clangTidyCustomModule clangTidyDarwinModule clangTidyFuchsiaModule
        clangTidyGoogleModule clangTidyHICPPModule clangTidyLinuxKernelModule
        clangTidyLLVMModule clangTidyLLVMLibcModule clangTidyMiscModule
        clangTidyModernizeModule clangTidyMPIModule clangTidyObjCModule
        clangTidyOpenMPModule clangTidyPerformanceModule
        clangTidyPortabilityModule clangTidyReadabilityModule
        clangTidyZirconModule
        clangTooling clangToolingCore
        clangToolingInclusions clangToolingInclusionsStdlib clangToolingSyntax
    )

    target_include_directories(llvm-libs INTERFACE
        ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})

    if(NOT BUILD_SHARED_LIBS)
        target_compile_definitions(llvm-libs INTERFACE CLANG_BUILD_STATIC=1)
    endif()
endfunction()
