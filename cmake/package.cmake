include_guard()

include(${CMAKE_CURRENT_LIST_DIR}/llvm_setup.cmake)

setup_llvm()

get_filename_component(LLVM_INSTALL_PATH "${LLVM_INSTALL_PATH}" ABSOLUTE)

if(NOT EXISTS "${LLVM_INSTALL_PATH}")
    message(FATAL_ERROR "Error: The specified LLVM_INSTALL_PATH does not exist: ${LLVM_INSTALL_PATH}")
endif()

# set llvm include and lib path
add_library(llvm-libs INTERFACE IMPORTED)

message(STATUS "LLVM include path: ${LLVM_INSTALL_PATH}/include")
# add to include directories
target_include_directories(llvm-libs INTERFACE "${LLVM_INSTALL_PATH}/include")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_link_directories(llvm-libs INTERFACE "${LLVM_INSTALL_PATH}/lib")
    target_link_libraries(llvm-libs INTERFACE
        LLVMSupport
        LLVMFrontendOpenMP
        LLVMOption
        clangAST
        clangASTMatchers
        clangBasic
        clangDependencyScanning
        clangDriver
        clangFormat
        clangFrontend
        clangIndex
        clangLex
        clangSema
        clangSerialization
        clangTidy
        clangTidyUtils
        # ALL_CLANG_TIDY_CHECKS
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
endif()


if(WIN32)
    target_compile_definitions(llvm-libs INTERFACE "CLANG_BUILD_STATIC")
    target_link_libraries(llvm-libs INTERFACE version ntdll)
endif()

# install dependencies
include(FetchContent)

if(WIN32)
    set(NULL_DEVICE NUL)
else()
    set(NULL_DEVICE /dev/null)
endif()

# libuv
FetchContent_Declare(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG v1.x
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ASAN ON CACHE BOOL "Enable AddressSanitizer for libuv" FORCE)
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libs")

# spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
)

# tomlplusplus
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
)

# croaring
FetchContent_Declare(
    croaring
    GIT_REPOSITORY https://github.com/RoaringBitmap/CRoaring.git
    GIT_TAG        v4.4.0
    # Workaround for https://github.com/RoaringBitmap/CRoaring/pull/750
    PATCH_COMMAND  git apply --reverse --check ${PROJECT_SOURCE_DIR}/cmake/croaring-fix.patch 2> ${NULL_DEVICE}
                    || git apply ${PROJECT_SOURCE_DIR}/cmake/croaring-fix.patch
)
set(ENABLE_ROARING_TESTS OFF CACHE INTERNAL "")

set(CMAKE_MODULE_PATH "")
FetchContent_MakeAvailable(libuv spdlog tomlplusplus croaring)

if(WIN32)
    target_compile_definitions(uv_a PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND TARGET uv_a)
    target_compile_options(uv_a PRIVATE
        "-Wno-unused-function"
        "-Wno-unused-variable"
        "-Wno-unused-but-set-variable"
        "-Wno-deprecated-declarations"
        "-Wno-missing-braces"
    )
endif()

target_compile_definitions(spdlog PUBLIC SPDLOG_USE_STD_FORMAT=1 SPDLOG_NO_EXCEPTIONS=1)
