cmake_minimum_required(VERSION 3.20)
project(CLICE_PROJECT LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

if(CLICE_USE_LIBCXX)
    string(APPEND CMAKE_CXX_FLAGS "-stdlib=libc++")
    string(APPEND CMAKE_EXE_LINKER_FLAGS "-stdlib=libc++")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS "-stdlib=libc++")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS "-stdlib=libc++")
endif()

include(cmake/package.cmake)

if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

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

if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/include")
    # private files are downloaded here
    message(STATUS "Fetched private headers to include: ${CMAKE_CURRENT_BINARY_DIR}/include")
    target_include_directories(llvm-libs INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/include")
endif()

if(WIN32)
    target_compile_definitions(llvm-libs INTERFACE "CLANG_BUILD_STATIC")
    target_link_libraries(llvm-libs INTERFACE version ntdll)
endif()

target_compile_options(clice-core PUBLIC ${CLICE_CXX_FLAGS})
target_link_options(clice-core PUBLIC ${CLICE_LINKER_FLAGS})

target_include_directories(clice-core PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(clice-core PUBLIC uv_a spdlog::spdlog tomlplusplus::tomlplusplus roaring::roaring llvm-libs)
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
