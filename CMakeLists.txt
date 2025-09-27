cmake_minimum_required(VERSION 3.20)
project(CLICE_PROJECT LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

include(cmake/llvm_setup.cmake)

setup_llvm()

get_filename_component(LLVM_INSTALL_PATH "${LLVM_INSTALL_PATH}" ABSOLUTE)

if(NOT EXISTS "${LLVM_INSTALL_PATH}")
    message(FATAL_ERROR "Error: The specified LLVM_INSTALL_PATH does not exist: ${LLVM_INSTALL_PATH}")
endif()
message(STATUS "Found llvm-libs ${LLVM_INSTALL_PATH}")
if (EXISTS "${LLVM_CMAKE_DIR}")
    list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
else()
    list(APPEND CMAKE_PREFIX_PATH "${LLVM_INSTALL_PATH}")
endif()
message(STATUS "CMake module path: ${CMAKE_MODULE_PATH}")
message(STATUS "CMake prefix path: ${CMAKE_PREFIX_PATH}")

if(CLICE_USE_LIBCXX)
    string(APPEND CMAKE_CXX_FLAGS "-stdlib=libc++")
    string(APPEND CMAKE_EXE_LINKER_FLAGS "-stdlib=libc++")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS "-stdlib=libc++")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS "-stdlib=libc++")
endif()

message(STATUS "Installing other dependencies, please wait...")
# install dependencies
include(FetchContent)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    # libuv option
    set(ASAN ON CACHE BOOL "" FORCE)
endif()

message(STATUS "Fetching libuv...")
FetchContent_Declare(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG v1.x
)

set(BUILD_SHARED_LIBS OFF)
message(STATUS "Fetching spdlog...")
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
)

message(STATUS "Fetching tomlplusplus...")
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
)

FetchContent_MakeAvailable(libuv spdlog tomlplusplus)

message(STATUS "Found tomlplusplus: ${tomlplusplus_SOURCE_DIR}")
message(STATUS "Found libuv: ${libuv_SOURCE_DIR}")
message(STATUS "Found spdlog: ${spdlog_SOURCE_DIR}")

if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    target_compile_definitions(uv_a PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_compile_definitions(spdlog PUBLIC SPDLOG_USE_STD_FORMAT=1 SPDLOG_NO_EXCEPTIONS=1)

if (MSVC)
    # Remove any RTTI or exception enabling flags from CMAKE_CXX_FLAGS
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

    # Fix MSVC Non-standard preprocessor caused error C1189
    # While compiling Command.cpp, MSVC won't expand Options macro correctly
    # Output: D:\Desktop\code\clice\build\.packages\l\llvm\20.1.5\cc2aa9f1d09a4b71b6fa3bf0011f6387\include\clang/Driver/Options.inc(3590): error C2365: “clang::driver::options::OPT_”: redefinition; previous definition was 'enumerator'
    set(CLICE_CXX_FLAGS "/GR-;/EHsc-;/Zc:preprocessor;")
else()
    set(CLICE_CXX_FLAGS "-fno-rtti;-fno-exceptions;-Wno-deprecated-declarations;-Wno-undefined-inline;")
endif()

set(CLICE_LINKER_FLAGS "")

# Build-type specific flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Debug build: Enabling -g -O0 -fsanitize=address")
    list(APPEND CLICE_CXX_FLAGS "-g;-O0;-fsanitize=address")
    list(APPEND CLICE_LINKER_FLAGS "-fsanitize=address")
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    if(NOT MSVC)
        # MSVC only supports ASAN
        message(STATUS "RelWithDebInfo build: Enabling -fsanitize=thread")
        list(APPEND CLICE_CXX_FLAGS "-fsanitize=thread")
        list(APPEND CLICE_LINKER_FLAGS "-fsanitize=thread")
    endif()
else()
    message(STATUS "Release/Default build: Enabling -O3")
    list(APPEND CLICE_CXX_FLAGS "-O3")
endif()

if(CLICE_CI_ENVIRONMENT)
    list(APPEND CLICE_CXX_FLAGS "-DCLICE_CI_ENVIRONMENT")
endif()


# Linker flags for LLD
if(WIN32)
    list(APPEND CLICE_LINKER_FLAGS "-fuse-ld=lld-link")
else()
    list(APPEND CLICE_LINKER_FLAGS "-fuse-ld=lld")
endif()

# build clice core part as library
file(GLOB_RECURSE CLICE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/AST/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/Async/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/Basic/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/Compiler/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/Index/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/Feature/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/Server/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/Support/*.cpp"
)
add_library(clice-core STATIC "${CLICE_SOURCES}")

# set llvm include and lib path
add_library(llvm-libs INTERFACE IMPORTED)

message(STATUS "LLVM include path: ${LLVM_INSTALL_PATH}/include")
# add to include directories
target_include_directories(llvm-libs INTERFACE "${LLVM_INSTALL_PATH}/include")

if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/include")
    # private files are downloaded here
    message(STATUS "Fetched private headers to include: ${CMAKE_CURRENT_BINARY_DIR}/include")
    target_include_directories(llvm-libs INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/include")
endif()

if(WIN32)
    target_compile_definitions(llvm-libs INTERFACE "CLANG_BUILD_STATIC")
    target_link_libraries(llvm-libs INTERFACE version ntdll)
endif()

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

target_compile_options(clice-core PUBLIC ${CLICE_CXX_FLAGS})
target_link_options(clice-core PUBLIC ${CLICE_LINKER_FLAGS})

target_include_directories(clice-core PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(clice-core PUBLIC uv_a spdlog::spdlog tomlplusplus::tomlplusplus llvm-libs)
target_compile_definitions(clice-core PUBLIC TOML_EXCEPTIONS=0)

# clice executable
add_executable(clice "${CMAKE_SOURCE_DIR}/src/Driver/clice.cc")
target_link_libraries(clice PRIVATE clice-core)
target_compile_options(clice PUBLIC ${CLICE_CXX_FLAGS})
target_link_options(clice PUBLIC ${CLICE_LINKER_FLAGS})
# install clice executable
install(TARGETS clice RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# copy resource dir
message(STATUS "Copying resource directory")
file(COPY
    ${LLVM_INSTALL_PATH}/lib/clang/20/
    DESTINATION ${PROJECT_BINARY_DIR}/lib/clang/20
)

# clice tests
if(CLICE_ENABLE_TEST)
    file(GLOB_RECURSE CLICE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/unit/*/*.cpp")
    add_executable(unit_tests "${CLICE_TEST_SOURCES}" "${CMAKE_SOURCE_DIR}/src/Driver/unit_tests.cc")
    target_include_directories(unit_tests PUBLIC "${CMAKE_SOURCE_DIR}")

    target_link_libraries(unit_tests PRIVATE clice-core)
    target_compile_options(unit_tests PUBLIC ${CLICE_CXX_FLAGS})
    target_link_options(unit_tests PUBLIC ${CLICE_LINKER_FLAGS})
endif()
