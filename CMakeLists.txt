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
option(CLICE_USE_LIBCXX "Use libc++ instead of libstdc++" OFF)
option(CLICE_OFFLINE_BUILD "Disable network downloads during configuration" OFF)
option(CLICE_ENABLE_TEST "Build unit tests" OFF)
option(CLICE_CI_ENVIRONMENT "Enable CI-specific configuration" OFF)
option(CLICE_ENABLE_BENCHMARK "Build benchmarks" OFF)
option(CLICE_RELEASE "Enable release packaging (LTO + strip + pack)" OFF)

# Global flags that apply to all targets (including FetchContent dependencies).
if(NOT MSVC)
    add_compile_options(-ffunction-sections -fdata-sections)
endif()

if(APPLE)
    # https://conda-forge.org/docs/maintainer/knowledge_base/#newer-c-features-with-old-sdk
    add_compile_definitions(_LIBCPP_DISABLE_AVAILABILITY=1)
endif()

if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
            CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,/OPT:REF")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,/OPT:REF")
elseif(APPLE)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,-dead_strip")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,-dead_strip")
else()
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -static-libstdc++ -static-libgcc -Wl,--gc-sections")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--gc-sections")
endif()

if(CLICE_USE_LIBCXX)
    string(APPEND CMAKE_CXX_FLAGS " -stdlib=libc++")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -stdlib=libc++")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -stdlib=libc++")
endif()

if(CLICE_RELEASE)
    set(CLICE_ENABLE_LTO ON)
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

    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # clang-cl (MSVC frontend): manually link ASan runtime since clang-cl
        # doesn't handle -fsanitize=address linking automatically.
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
            string(APPEND CMAKE_EXE_LINKER_FLAGS " ${flag}")
            string(APPEND CMAKE_SHARED_LINKER_FLAGS " ${flag}")
            string(APPEND CMAKE_MODULE_LINKER_FLAGS " ${flag}")
        endforeach()
    else()
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=address")
        string(APPEND CMAKE_SHARED_LINKER_FLAGS " -fsanitize=address")
    endif()

    if(WIN32)
        # Disable Identical COMDAT Folding in Debug to avoid ASan ODR false positives.
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,/OPT:NOICF")
        string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,/OPT:NOICF")
    endif()
endif()

include("${PROJECT_SOURCE_DIR}/cmake/package.cmake")

# Project-specific options (not applied to third-party deps).
add_library(clice_options INTERFACE)

if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
         CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(clice_options INTERFACE /GR- /EHs-c- /Zc:preprocessor)
else()
    target_compile_options(clice_options INTERFACE
        -fno-rtti
        -fno-exceptions
        -Wno-deprecated-declarations
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wno-undefined-inline>
    )
endif()

if(WIN32)
    target_link_libraries(clice_options INTERFACE version ntdll)
endif()

if(CLICE_ENABLE_TEST)
    target_compile_definitions(clice_options INTERFACE CLICE_ENABLE_TEST=1)
endif()

if(CLICE_CI_ENVIRONMENT)
    target_compile_definitions(clice_options INTERFACE CLICE_CI_ENVIRONMENT=1)
endif()

file(GLOB_RECURSE CLICE_CORE_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.cpp")
add_library(clice-core STATIC ${CLICE_CORE_SOURCES})
add_library(clice::core ALIAS clice-core)

target_include_directories(clice-core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(clice-core PUBLIC
    clice_options
    llvm-libs
    spdlog::spdlog
    roaring::roaring
    kota::ipc::lsp
    kota::codec::toml
    kota::codec::flatbuffers
    simdjson::simdjson
)

add_executable(clice "${PROJECT_SOURCE_DIR}/src/clice.cc")
target_link_libraries(clice PRIVATE clice::core kota::deco)
install(TARGETS clice RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_custom_target(copy_clang_resource ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${LLVM_INSTALL_PATH}/lib/clang"
        "${PROJECT_BINARY_DIR}/lib/clang"
    COMMENT "Copying clang resource directory"
)
install(
    DIRECTORY "${LLVM_INSTALL_PATH}/lib/clang"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

if(CLICE_ENABLE_TEST)
    file(GLOB_RECURSE CLICE_TEST_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tests/unit/*/*_tests.cpp"
    )
    set(CLICE_TEST_SUPPORT_SOURCES
        "${PROJECT_SOURCE_DIR}/tests/unit/test/annotation.cpp"
        "${PROJECT_SOURCE_DIR}/tests/unit/test/tester.cpp"
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
    target_link_libraries(unit_tests PRIVATE clice::core kota::zest kota::deco)
endif()

if(CLICE_ENABLE_BENCHMARK)
    add_executable(scan_benchmark
        "${PROJECT_SOURCE_DIR}/benchmarks/scan_benchmark.cpp"
    )
    target_include_directories(scan_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(scan_benchmark PRIVATE clice::core kota::deco)
endif()

if(CLICE_RELEASE)
    include("${PROJECT_SOURCE_DIR}/cmake/release.cmake")
endif()
