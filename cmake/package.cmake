include_guard()

include(${CMAKE_CURRENT_LIST_DIR}/llvm_setup.cmake)

setup_llvm("21.1.4")

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
        LLVMTargetParser
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

if(NOT WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ASAN ON CACHE BOOL "Enable AddressSanitizer for libuv" FORCE)
endif()
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

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
    GIT_TAG        v4.4.2
)
set(ENABLE_ROARING_TESTS OFF CACHE INTERNAL "" FORCE)

# flatbuffers
FetchContent_Declare(
    flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v25.9.23
)
set(FLATBUFFERS_BUILD_GRPC OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(libuv spdlog tomlplusplus croaring flatbuffers)

if(WIN32)
    target_compile_definitions(uv_a PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(NOT MSVC AND TARGET uv_a)
    target_compile_options(uv_a PRIVATE
        "-Wno-unused-function"
        "-Wno-unused-variable"
        "-Wno-unused-but-set-variable"
        "-Wno-deprecated-declarations"
        "-Wno-missing-braces"
    )
endif()

target_compile_definitions(spdlog PUBLIC
    SPDLOG_USE_STD_FORMAT=1
    SPDLOG_NO_EXCEPTIONS=1
)
