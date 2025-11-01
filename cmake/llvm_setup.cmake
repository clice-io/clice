include_guard()

include(${CMAKE_CURRENT_LIST_DIR}/github.cmake)

# Check if LLVM version is supported
function(check_llvm_version LOCAL_LLVM_VERSION LLVM_VERSION OUTPUT_VAR)
    if((NOT DEFINED LOCAL_LLVM_VERSION) OR (LOCAL_LLVM_VERSION STREQUAL ""))
        message(WARNING "LLVM version is not set.")
        set(${OUTPUT_VAR} FALSE PARENT_SCOPE)
    elseif(NOT (LOCAL_LLVM_VERSION VERSION_GREATER_EQUAL LLVM_VERSION))
        message(WARNING "Unsupported LLVM version: ${LOCAL_LLVM_VERSION}. Only LLVM ${LLVM_VERSION} is supported.")
        set(${OUTPUT_VAR} FALSE PARENT_SCOPE)
    else()
        set(${OUTPUT_VAR} TRUE PARENT_SCOPE)
    endif()
endfunction()

# Look up LLVM version's corresponding commit SHA
function(lookup_commit LLVM_VERSION OUTPUT_VAR)
    set(LLVM_TAG "llvmorg-${LLVM_VERSION}")
    github_lookup_tag_commit("llvm" "llvm-project" ${LLVM_TAG} COMMIT_SHA)
    set(${OUTPUT_VAR} ${COMMIT_SHA} PARENT_SCOPE)
endfunction()

# Fetch private Clang header files from LLVM source
function(fetch_private_clang_files LLVM_VERSION)
    set(PRIVATE_CLANG_FILE_LIST
        "Sema/CoroutineStmtBuilder.h"
        "Sema/TypeLocBuilder.h"
        "Sema/TreeTransform.h"
    )

    # Check if all files already exist
    set(PRIVATE_FILE_EXISTS TRUE)
    foreach(FILE ${PRIVATE_CLANG_FILE_LIST})
        if(EXISTS "${LLVM_INSTALL_PATH}/include/clang/${FILE}")
            message(STATUS "Private clang file found in LLVM installation: ${FILE}")
        elseif(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/include/clang/${FILE}")
            message(STATUS "Private clang file already exists: ${FILE}")
        else()
            set(PRIVATE_FILE_EXISTS FALSE)
            break()
        endif()
    endforeach()

    if(PRIVATE_FILE_EXISTS)
        message(STATUS "All required private clang files already exist.")
        return()
    endif()

    # Skip download in offline build mode
    if(CLICE_OFFLINE_BUILD)
        message(WARNING "CLICE_OFFLINE_BUILD is enabled, skipping private clang files download")
        message(WARNING "Build may fail if required private headers are missing")
        return()
    endif()

    message(WARNING "Required private clang files incomplete, fetching from llvm-project source...")

    # Get the commit SHA for this LLVM version
    lookup_commit(${LLVM_VERSION} LLVM_COMMIT)
    if(LLVM_COMMIT STREQUAL "NOTFOUND")
        message(WARNING "Failed to lookup commit for LLVM ${LLVM_VERSION}, skipping private clang files download")
        return()
    endif()

    message(STATUS "LLVM ${LLVM_VERSION} corresponds to commit ${LLVM_COMMIT}")

    # Download missing files
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include/clang")
    foreach(FILE ${PRIVATE_CLANG_FILE_LIST})
        set(FILE_PATH "${CMAKE_CURRENT_BINARY_DIR}/include/clang/${FILE}")
        if(NOT EXISTS "${FILE_PATH}")
            message(STATUS "Downloading ${FILE}...")
            file(DOWNLOAD "https://raw.githubusercontent.com/llvm/llvm-project/${LLVM_COMMIT}/clang/lib/${FILE}"
                         "${FILE_PATH}"
                         STATUS DOWNLOAD_STATUS
                         TLS_VERIFY ON)
            list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
            if(NOT STATUS_CODE EQUAL 0)
                message(FATAL_ERROR "Failed to download private clang file: ${FILE}")
            endif()
        endif()
    endforeach()
endfunction()

# Detect system-installed LLVM using llvm-config
function(detect_llvm OUTPUT_VAR)
    find_program(LLVM_CONFIG_EXEC llvm-config)

    if(NOT LLVM_CONFIG_EXEC)
        set(${OUTPUT_VAR} "" PARENT_SCOPE)
        return()
    endif()

    # Get LLVM version and paths
    execute_process(
        COMMAND "${LLVM_CONFIG_EXEC}" --version
        OUTPUT_VARIABLE LLVM_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND "${LLVM_CONFIG_EXEC}" --prefix
        OUTPUT_VARIABLE LLVM_INSTALL_PATH_DETECTED
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND "${LLVM_CONFIG_EXEC}" --cmakedir
        OUTPUT_VARIABLE LLVM_CMAKE_DIR_DETECTED
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Set cache variables
    set(LLVM_INSTALL_PATH "${LLVM_INSTALL_PATH_DETECTED}" CACHE PATH "Path to LLVM installation" FORCE)
    set(LLVM_CMAKE_DIR "${LLVM_CMAKE_DIR_DETECTED}" CACHE PATH "Path to LLVM CMake files" FORCE)
    set(${OUTPUT_VAR} ${LLVM_VERSION} PARENT_SCOPE)
endfunction()

# Download and install prebuilt LLVM binaries with error checking
function(install_prebuilt_llvm LLVM_VERSION)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.llvm")

    # Determine platform-specific package name
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(LLVM_BUILD_TYPE "debug")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set(LLVM_BUILD_TYPE "releasedbg")
    else()
        set(LLVM_BUILD_TYPE "release-lto")
    endif()

    if(WIN32)
        set(LLVM_PACKAGE "x64-windows-msvc-${LLVM_BUILD_TYPE}.7z")
    elseif(APPLE)
        set(LLVM_PACKAGE "arm64-macosx-apple-${LLVM_BUILD_TYPE}.tar.xz")
    elseif(UNIX)
        set(LLVM_PACKAGE "x86_64-linux-gnu-${LLVM_BUILD_TYPE}.tar.xz")
    endif()

    if(NOT LLVM_PACKAGE)
        message(FATAL_ERROR "Unsupported platform or build type for prebuilt LLVM.")
    endif()

    set(DOWNLOAD_PATH "${CMAKE_CURRENT_BINARY_DIR}/${LLVM_PACKAGE}")

    # Download if file does not exist
    if(NOT EXISTS "${DOWNLOAD_PATH}")
        message(STATUS "Downloading prebuilt LLVM package: ${LLVM_PACKAGE}")
        set(DOWNLOAD_URL "https://github.com/clice-io/llvm-binary/releases/download/${LLVM_VERSION}/${LLVM_PACKAGE}")
        file(DOWNLOAD "${DOWNLOAD_URL}"
                      "${DOWNLOAD_PATH}"
                      STATUS DOWNLOAD_STATUS
                      SHOW_PROGRESS
                      TLS_VERIFY ON)
        list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
        list(GET DOWNLOAD_STATUS 1 ERROR_MESSAGE)

        if(NOT STATUS_CODE EQUAL 0)
            # Download failed, remove the incomplete file to force a fresh download next time
            if(EXISTS "${DOWNLOAD_PATH}")
                file(REMOVE "${DOWNLOAD_PATH}")
                message(STATUS "Removed incomplete file: ${DOWNLOAD_PATH}")
            endif()
            message(FATAL_ERROR "Failed to download prebuilt LLVM package from ${DOWNLOAD_URL}.\nError: ${ERROR_MESSAGE}")
        endif()
    else()
        message(STATUS "Prebuilt LLVM package already exists, skipping download.")
    endif()

    message(STATUS "Extracting LLVM package: ${LLVM_PACKAGE}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xvf "${DOWNLOAD_PATH}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.llvm"
        RESULT_VARIABLE TAR_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )

    if(NOT TAR_RESULT EQUAL "0")
        message(FATAL_ERROR "Failed to extract archive. The file may be corrupted or the tool is missing.")
    endif()

    # Set installation paths
    set(LLVM_INSTALL_PATH "${CMAKE_CURRENT_BINARY_DIR}/.llvm" CACHE PATH "Path to LLVM installation" FORCE)
    set(LLVM_CMAKE_DIR "${LLVM_INSTALL_PATH}/lib/cmake/llvm" CACHE PATH "Path to LLVM CMake files" FORCE)
    message(STATUS "LLVM installation path set to: ${LLVM_INSTALL_PATH}")
endfunction()

# Main function to set up LLVM for the project
function(setup_llvm LLVM_VERSION)
    if(NOT DEFINED LLVM_VERSION OR LLVM_VERSION STREQUAL "")
        message(FATAL_ERROR "setup_llvm() requires a LLVM_VERSION argument (e.g., '21.1.4').")
    endif()

    # Use existing LLVM installation if path is already set
    if(DEFINED LLVM_INSTALL_PATH AND NOT LLVM_INSTALL_PATH STREQUAL "")
        message(STATUS "LLVM_INSTALL_PATH is set to ${LLVM_INSTALL_PATH}, using it directly.")
        return()
    endif()

    set(LLVM_VERSION_OK false)
    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        # Try to detect system LLVM
        detect_llvm(LOCAL_LLVM_VERSION)
        check_llvm_version("${LOCAL_LLVM_VERSION}" "${LLVM_VERSION}" LLVM_VERSION_OK)
    endif()

    # Download prebuilt LLVM if system version is not suitable
    if(NOT LLVM_VERSION_OK)
        message(WARNING "System LLVM not found, version mismatch or incompatible build type.")
        message(WARNING "Downloading prebuilt LLVM ${LLVM_VERSION} ...")
        install_prebuilt_llvm("${LLVM_VERSION}")
    endif()

    # Fetch required private Clang headers
    fetch_private_clang_files("${LLVM_VERSION}")
endfunction()
