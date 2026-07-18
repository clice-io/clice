cmake_minimum_required(VERSION 3.30)

# Base version; the released version string comes from git describe (see
# cmake/generate_version.cmake) and this value is the tarball fallback.
project(CLICE_PROJECT VERSION 0.1.0 LANGUAGES C CXX)

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

# Global flags that apply to all targets (including FetchContent dependencies).
if(NOT MSVC)
    add_compile_options(-ffunction-sections -fdata-sections)
endif()

if(APPLE)
    # https://conda-forge.org/docs/maintainer/knowledge_base/#newer-c-features-with-old-sdk
    add_compile_definitions(_LIBCPP_DISABLE_AVAILABILITY=1)
endif()

# Keyed on the target OS, not the compiler frontend: CI links Windows with
# GNU-driver clang++ + lld-link, where the frontend variant is "GNU" but the
# linker still speaks MSVC-style /OPT flags (see the Debug /OPT:NOICF below).
if(WIN32)
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

# Releases deliberately do NOT enable LTO yet: ThinLTO's cross-TU inlining
# defeats kota's out-of-line workaround for llvm-project#105595 (clang < 21
# drops coroutine promise policy writes under -O3), which silently breaks
# cancellation — six cancellation unit tests fail on any ThinLTO build.
# Re-enable together with the clang 21 toolchain upgrade.

if(CLICE_ENABLE_LTO)
    string(APPEND CMAKE_C_FLAGS " -flto=thin")
    string(APPEND CMAKE_CXX_FLAGS " -flto=thin")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -flto=thin")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -flto=thin")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS " -flto=thin")
endif()

# Link optimizations are the default for optimized builds: what the test
# suites run is byte-for-byte what releases ship. Debug keeps the ASan
# friendly flags below instead. icf=safe (not =all) so address-taken
# functions keep C++ pointer identity.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(WIN32)
        # SAFEICF, not ICF: plain /OPT:ICF folds address-taken functions.
        set(CLICE_LINK_OPT_FLAGS "-Wl,/OPT:SAFEICF")
    elseif(APPLE)
        # --keep-icf-stabs: without it dsymutil cannot see ICF-folded symbols,
        # so they would be missing from the dSYM (and thus from GSYM).
        set(CLICE_LINK_OPT_FLAGS "-Wl,--icf=safe" "-Wl,--keep-icf-stabs")
    else()
        set(CLICE_LINK_OPT_FLAGS "-Wl,--icf=safe" "-Wl,-O2")
    endif()

    # Only lld understands these; manual builds on the system linker
    # (ld.bfd, Apple ld64) build fine without them.
    include(CheckLinkerFlag)
    check_linker_flag(CXX "${CLICE_LINK_OPT_FLAGS}" CLICE_LINKER_OPT_SUPPORTED)
    if(CLICE_LINKER_OPT_SUPPORTED)
        string(JOIN " " CLICE_LINK_OPT_FLAGS ${CLICE_LINK_OPT_FLAGS})
        string(APPEND CMAKE_EXE_LINKER_FLAGS " ${CLICE_LINK_OPT_FLAGS}")
    else()
        message(STATUS "Linker rejects ${CLICE_LINK_OPT_FLAGS}; skipping link optimizations")
    endif()
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

set(FBS_SCHEMA_FILE "${PROJECT_SOURCE_DIR}/src/index/schema.fbs")
set(GENERATED_HEADER "${PROJECT_BINARY_DIR}/generated/schema_generated.h")

if(CMAKE_CROSSCOMPILING)
    find_program(FLATC_EXECUTABLE flatc REQUIRED)
    set(FLATC_CMD "${FLATC_EXECUTABLE}")
else()
    set(FLATC_CMD "$<TARGET_FILE:flatc>")
endif()

add_custom_command(
    OUTPUT "${GENERATED_HEADER}"
    COMMAND ${FLATC_CMD} --cpp -o "${PROJECT_BINARY_DIR}/generated" "${FBS_SCHEMA_FILE}"
    DEPENDS "${FBS_SCHEMA_FILE}"
    COMMENT "Generating C++ header from ${FBS_SCHEMA_FILE}"
)

add_custom_target(generate_flatbuffers_schema DEPENDS "${GENERATED_HEADER}")

# Version header, regenerated on every build so the embedded git describe
# tracks the checked-out commit (content-unchanged writes are elided).
set(CLICE_VERSION_HEADER "${PROJECT_BINARY_DIR}/generated/version.h")
# Target identifier stamped next to the version: the explicit triple for
# cross builds (see cmake/toolchain.cmake); native builds compose the same
# arch-os-abi form the release asset names use, so a crash log names the
# exact downloadable artifact.
if(DEFINED CLICE_TARGET_TRIPLE)
    string(TOLOWER "${CLICE_TARGET_TRIPLE}" CLICE_TARGET_RAW)
else()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}" CLICE_TARGET_RAW)
endif()
if(CLICE_TARGET_RAW MATCHES "^(x86_64|amd64|x64)")
    set(CLICE_TARGET_ARCH "x64")
else()
    set(CLICE_TARGET_ARCH "arm64")
endif()
if(CLICE_TARGET_RAW MATCHES "windows")
    set(CLICE_TARGET_STRING "${CLICE_TARGET_ARCH}-windows-msvc")
elseif(CLICE_TARGET_RAW MATCHES "darwin|apple|macos")
    set(CLICE_TARGET_STRING "${CLICE_TARGET_ARCH}-macos-darwin")
else()
    set(CLICE_TARGET_STRING "${CLICE_TARGET_ARCH}-linux-gnu")
endif()
add_custom_target(generate_version_header
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DFALLBACK_VERSION=${PROJECT_VERSION}
        -DTARGET_STRING=${CLICE_TARGET_STRING}
        -DTEMPLATE=${PROJECT_SOURCE_DIR}/cmake/version.h.in
        -DOUTPUT_FILE=${CLICE_VERSION_HEADER}
        -P "${PROJECT_SOURCE_DIR}/cmake/generate_version.cmake"
    BYPRODUCTS "${CLICE_VERSION_HEADER}"
    COMMENT "Generating version header"
)

file(GLOB_RECURSE CLICE_CORE_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.cpp")
add_library(clice-core STATIC ${CLICE_CORE_SOURCES})
add_library(clice::core ALIAS clice-core)
add_dependencies(clice-core generate_flatbuffers_schema generate_version_header)

target_include_directories(clice-core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/generated"
)
target_link_libraries(clice-core PUBLIC
    clice_options
    llvm-libs
    spdlog::spdlog
    roaring::roaring
    flatbuffers
    kota::ipc::lsp
    kota::codec::toml
    kota::option
    simdjson::simdjson
)

add_executable(clice "${PROJECT_SOURCE_DIR}/src/clice.cc")
target_link_libraries(clice PRIVATE clice::core kota::deco)
add_dependencies(clice generate_version_header)
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
    # The generated/ include dir propagates through clice::core, but the
    # generation order does not — depend explicitly so a test including
    # version.h cannot race the generator on a clean parallel build.
    add_dependencies(unit_tests generate_version_header)
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

include("${PROJECT_SOURCE_DIR}/cmake/release.cmake")
