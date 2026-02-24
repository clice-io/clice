cmake_minimum_required(VERSION 3.30)

project(CLICE_PROJECT LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

option(CLICE_ENABLE_LTO "Enable ThinLTO for all targets" OFF)

# Make sure all third libraries are affected by ABI related options
if(CLICE_USE_LIBCXX)
    string(APPEND CMAKE_CXX_FLAGS " -stdlib=libc++")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -stdlib=libc++")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -stdlib=libc++")
endif()

if(CLICE_ENABLE_LTO)
    string(APPEND CMAKE_C_FLAGS " -flto=thin")
    string(APPEND CMAKE_CXX_FLAGS " -flto=thin")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -flto=thin")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -flto=thin")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS " -flto=thin")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address)

    if(NOT WIN32)
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=address")
        string(APPEND CMAKE_SHARED_LINKER_FLAGS " -fsanitize=address")
    endif()

    if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} --print-resource-dir
            OUTPUT_VARIABLE CLANG_RESOURCE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(ASAN_LIB_PATH "${CLANG_RESOURCE_DIR}/lib/windows")
        link_directories(${ASAN_LIB_PATH})

        set(ASAN_LINK_FLAGS "")

        list(APPEND ASAN_LINK_FLAGS "clang_rt.asan_dynamic-x86_64.lib")
        list(APPEND ASAN_LINK_FLAGS "/wholearchive:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")

        foreach(flag ${ASAN_LINK_FLAGS})
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${flag}")
            set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${flag}")
            set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${flag}")
        endforeach()
    endif()
endif()

if(APPLE)
    # https://conda-forge.org/docs/maintainer/knowledge_base/#newer-c-features-with-old-sdk
    string(APPEND CMAKE_CXX_FLAGS " -D_LIBCPP_DISABLE_AVAILABILITY=1")
endif()

include("${PROJECT_SOURCE_DIR}/cmake/package.cmake")

add_library(clice_options INTERFACE)

if(CLICE_ENABLE_TEST)
    target_compile_definitions(clice_options INTERFACE CLICE_ENABLE_TEST=1)
endif()

if(CLICE_CI_ENVIRONMENT)
    target_compile_definitions(clice_options INTERFACE CLICE_CI_ENVIRONMENT=1)
endif()

if(WIN32)
    target_link_libraries(clice_options INTERFACE version ntdll)
endif()

if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    target_link_options(clice_options INTERFACE
        -fuse-ld=lld-link
        -Wl,/OPT:REF
        #,/OPT:NOICF
    )
elseif(APPLE)
    target_link_options(clice_options INTERFACE
        -fuse-ld=lld
        -Wl,-dead_strip
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(clice_options INTERFACE
        -fuse-ld=lld
        -static-libstdc++ -static-libgcc
        -Wl,--gc-sections
    )
endif()

if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
            CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(clice_options INTERFACE
        /GR-
        /EHsc-
        /Zc:preprocessor
    )
else()
    target_compile_options(clice_options INTERFACE
        -fno-rtti
        -fno-exceptions
        -Wno-deprecated-declarations
        -Wno-undefined-inline
        -ffunction-sections
        -fdata-sections
    )
endif()

# Temporary migration-only build graph.
add_library(clice-core STATIC
    "${PROJECT_SOURCE_DIR}/src/support/logging.cpp"
    "${PROJECT_SOURCE_DIR}/src/syntax/lexer.cpp"
    "${PROJECT_SOURCE_DIR}/src/semantic/resolver.cpp"
)
add_library(clice::core ALIAS clice-core)

target_include_directories(clice-core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(clice-core PUBLIC
    clice_options
    llvm-libs
    spdlog::spdlog
    eventide::async
    eventide::language
)

add_executable(clice "${PROJECT_SOURCE_DIR}/src/clice.cc")
target_link_libraries(clice PRIVATE clice::core)
install(TARGETS clice RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(CLICE_ENABLE_TEST)
    file(GLOB_RECURSE CLICE_TEST_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tests/unit/*/*_tests.cpp"
        "${PROJECT_SOURCE_DIR}/tests/unit/*/*Tests.cpp"
    )
    set(CLICE_TEST_SUPPORT_SOURCES
        "${PROJECT_SOURCE_DIR}/tests/unit/test/annotation.cpp"
    )

    add_executable(unit_tests
        "${PROJECT_SOURCE_DIR}/tests/unit/unit_tests.cc"
        ${CLICE_TEST_SOURCES}
        ${CLICE_TEST_SUPPORT_SOURCES}
    )
    target_include_directories(unit_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(unit_tests PRIVATE clice::core eventide::zest)
endif()
