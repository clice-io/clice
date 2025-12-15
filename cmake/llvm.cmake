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

    # set llvm include and lib path
    add_library(llvm-libs INTERFACE IMPORTED)

    # add to include directories
    target_include_directories(llvm-libs INTERFACE "${LLVM_INSTALL_PATH}/include")

    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT WIN32)
        target_link_directories(llvm-libs INTERFACE "${LLVM_INSTALL_PATH}/lib")
        target_link_libraries(llvm-libs INTERFACE
            LLVMSupport
            LLVMFrontendOpenMP
            LLVMOption
            LLVMTargetParser
            clangAST
            clangASTMatchers
            clangBasic
            clangDriver
            clangFormat
            clangFrontend
            clangLex
            clangSema
            clangSerialization
            clangTidy
            clangTidyUtils
            clangTidyAndroidModule
            clangTidyAbseilModule
            clangTidyAlteraModule
            clangTidyBoostModule
            clangTidyBugproneModule
            clangTidyCERTModule
            clangTidyConcurrencyModule
            clangTidyCppCoreGuidelinesModule
            clangTidyDarwinModule
            clangTidyFuchsiaModule
            clangTidyGoogleModule
            clangTidyHICPPModule
            clangTidyLinuxKernelModule
            clangTidyLLVMModule
            clangTidyLLVMLibcModule
            clangTidyMiscModule
            clangTidyModernizeModule
            clangTidyObjCModule
            clangTidyOpenMPModule
            clangTidyPerformanceModule
            clangTidyPortabilityModule
            clangTidyReadabilityModule
            clangTidyZirconModule
            clangTooling
            clangToolingCore
            clangToolingInclusions
            clangToolingInclusionsStdlib
            clangToolingSyntax
        )
    else()
        file(GLOB LLVM_LIBRARIES CONFIGURE_DEPENDS "${LLVM_INSTALL_PATH}/lib/*${CMAKE_STATIC_LIBRARY_SUFFIX}")
        target_link_libraries(llvm-libs INTERFACE ${LLVM_LIBRARIES})
        target_compile_definitions(llvm-libs INTERFACE CLANG_BUILD_STATIC=1)
    endif()
endfunction()
