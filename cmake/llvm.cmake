include_guard()

function(setup_llvm LLVM_VERSION)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(LLVM_SETUP_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/.llvm/setup-llvm.json")
    set(LLVM_SETUP_SCRIPT "${PROJECT_SOURCE_DIR}/scripts/setup-llvm.py")
    set(LLVM_SETUP_ARGS
        "--version" "${LLVM_VERSION}"
        "--build-type" "${CMAKE_BUILD_TYPE}"
        "--binary-dir" "${CMAKE_CURRENT_BINARY_DIR}"
        "--manifest" "${PROJECT_SOURCE_DIR}/config/llvm-manifest.json"
        "--output" "${LLVM_SETUP_OUTPUT}"
    )

    if(CLICE_ENABLE_LTO)
        list(APPEND LLVM_SETUP_ARGS "--enable-lto")
    endif()

    if(DEFINED LLVM_INSTALL_PATH AND NOT LLVM_INSTALL_PATH STREQUAL "")
        list(APPEND LLVM_SETUP_ARGS "--install-path" "${LLVM_INSTALL_PATH}")
    endif()

    if(DEFINED CLICE_OFFLINE_BUILD AND CLICE_OFFLINE_BUILD)
        list(APPEND LLVM_SETUP_ARGS "--offline")
    endif()

    if(DEFINED CLICE_TARGET_TRIPLE)
        if(CLICE_TARGET_TRIPLE MATCHES "linux")
            list(APPEND LLVM_SETUP_ARGS "--target-platform" "Linux")
        elseif(CLICE_TARGET_TRIPLE MATCHES "darwin")
            list(APPEND LLVM_SETUP_ARGS "--target-platform" "macosx")
        elseif(CLICE_TARGET_TRIPLE MATCHES "windows")
            list(APPEND LLVM_SETUP_ARGS "--target-platform" "Windows")
        endif()

        if(CLICE_TARGET_TRIPLE MATCHES "^aarch64")
            list(APPEND LLVM_SETUP_ARGS "--target-arch" "arm64")
        elseif(CLICE_TARGET_TRIPLE MATCHES "^x86_64")
            list(APPEND LLVM_SETUP_ARGS "--target-arch" "x64")
        endif()
    endif()

    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${LLVM_SETUP_SCRIPT}" ${LLVM_SETUP_ARGS}
        RESULT_VARIABLE LLVM_SETUP_RESULT
        OUTPUT_VARIABLE LLVM_SETUP_STDOUT
        ERROR_VARIABLE LLVM_SETUP_STDERR
        ECHO_OUTPUT_VARIABLE
        ECHO_ERROR_VARIABLE
        COMMAND_ERROR_IS_FATAL ANY
    )

    file(READ "${LLVM_SETUP_OUTPUT}" LLVM_SETUP_JSON)
    string(JSON LLVM_INSTALL_PATH GET "${LLVM_SETUP_JSON}" install_path)
    string(JSON LLVM_CMAKE_DIR GET "${LLVM_SETUP_JSON}" cmake_dir)
    set(LLVM_INSTALL_PATH "${LLVM_INSTALL_PATH}" CACHE PATH "Path to LLVM installation" FORCE)
    set(LLVM_CMAKE_DIR "${LLVM_CMAKE_DIR}" CACHE PATH "Path to LLVM CMake files" FORCE)

    get_filename_component(LLVM_INSTALL_PATH "${LLVM_INSTALL_PATH}" ABSOLUTE)

    if(NOT EXISTS "${LLVM_INSTALL_PATH}")
        message(FATAL_ERROR "Error: The specified LLVM_INSTALL_PATH does not exist: ${LLVM_INSTALL_PATH}")
    endif()

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
