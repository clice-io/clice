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
option(CLICE_OFFLINE_BUILD "Disable network downloads during configuration" OFF)
option(CLICE_ENABLE_TEST "Build unit tests" OFF)
option(CLICE_CI_ENVIRONMENT "Enable CI-specific configuration" OFF)
option(CLICE_ENABLE_BENCHMARK "Build benchmarks" OFF)

# Global flags that apply to all targets (including CPM dependencies).
if(NOT MSVC)
    add_compile_options(-ffunction-sections -fdata-sections)
endif()

if(APPLE)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,-dead_strip")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,-dead_strip")
else()
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -static-libgcc -Wl,--gc-sections")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--gc-sections")
endif()

# CRoaring's C files define the inline functions of its headers (C99
# `extern inline`), and C++ code calling them emits them again as COMDATs.
# ELF lets the strong definition win over the COMDAT copies; COFF reports
# both, so MinGW links take the first.
if(MINGW)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--allow-multiple-definition")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--allow-multiple-definition")
endif()

# Releases deliberately do NOT enable LTO yet. LLVM 22 removed the old
# toolchain blocker, but enabling remains deferred until the cancellation
# regression suite has been validated under ThinLTO.

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
    if(APPLE)
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

# Debug builds are ASan builds where xclang has the ASan libclang to link
# (cmake/llvm.cmake).
include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
set(CLICE_ENABLE_ASAN OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    clice_asan_available(CLICE_ENABLE_ASAN)
endif()
if(CLICE_ENABLE_ASAN)
    add_compile_options(-fsanitize=address)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=address")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -fsanitize=address")
endif()

include("${PROJECT_SOURCE_DIR}/cmake/package.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/pch.cmake")

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

# Version header, regenerated on every build so the embedded git describe
# tracks the checked-out commit (content-unchanged writes are elided).
set(CLICE_VERSION_HEADER "${PROJECT_BINARY_DIR}/generated/version.h")
# Target identifier stamped next to the version: the same canonical
# triple the release asset names use, so a crash log names the exact
# downloadable artifact.
clice_target_triple(CLICE_TARGET_STRING)
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

# Layered targets: core <- config <- {project, worker} <- sched <- server.
# Each layer may include downward only; the link DAG plus the layering check
# in CI (`pixi run check-layers`) enforce the boundary — sched and below
# never see an LSP or server header, and a project never sees scheduling.
file(GLOB_RECURSE CLICE_CORE_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/support/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/vfs/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/syntax/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/command/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/compile/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/semantic/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/index/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/feature/*.cpp"
)
add_library(clice-core STATIC ${CLICE_CORE_SOURCES})
add_library(clice::core ALIAS clice-core)
add_dependencies(clice-core generate_version_header)
clice_precompile_headers(clice-core)

target_include_directories(clice-core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/cmake"
    "${PROJECT_BINARY_DIR}/generated"
)
target_link_libraries(clice-core PUBLIC
    clice_options
    llvm-libs
    spdlog::spdlog
    roaring::roaring
    kota::codec::flatbuffers
    kota::ipc::lsp
    kota::codec::toml
    kota::option
    kota::support
    simdjson::simdjson
    lmdb
)

file(GLOB_RECURSE CLICE_CONFIG_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/config/*.cpp")
add_library(clice-config STATIC ${CLICE_CONFIG_SOURCES})
add_library(clice::config ALIAS clice-config)
target_link_libraries(clice-config PUBLIC clice::core)
clice_precompile_headers(clice-config)

file(GLOB_RECURSE CLICE_PROJECT_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/project/*.cpp")
add_library(clice-project STATIC ${CLICE_PROJECT_SOURCES})
add_library(clice::project ALIAS clice-project)
target_link_libraries(clice-project PUBLIC clice::config)
clice_precompile_headers(clice-project)

file(GLOB_RECURSE CLICE_WORKER_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/worker/*.cpp")
add_library(clice-worker STATIC ${CLICE_WORKER_SOURCES})
add_library(clice::worker ALIAS clice-worker)
target_link_libraries(clice-worker PUBLIC clice::config)
clice_precompile_headers(clice-worker)

file(GLOB_RECURSE CLICE_SCHED_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/sched/*.cpp")
add_library(clice-sched STATIC ${CLICE_SCHED_SOURCES})
add_library(clice::sched ALIAS clice-sched)
target_link_libraries(clice-sched PUBLIC clice::project clice::worker)
clice_precompile_headers(clice-sched)

file(GLOB_RECURSE CLICE_SERVER_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/server/*.cpp")
add_library(clice-server STATIC ${CLICE_SERVER_SOURCES})
add_library(clice::server ALIAS clice-server)
# Server sources include the generated version.h; a link dependency alone
# does not order another target's compilation after the custom command.
add_dependencies(clice-server generate_version_header)
target_link_libraries(clice-server PUBLIC clice::sched)
clice_precompile_headers(clice-server)

file(GLOB CLICE_DRIVER_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/driver/*.cc")
add_executable(clice "${PROJECT_SOURCE_DIR}/src/clice.cc" ${CLICE_DRIVER_SOURCES})
target_link_libraries(clice PRIVATE clice::server kota::deco)
clice_precompile_headers(clice)
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
    target_link_libraries(unit_tests PRIVATE clice::server kota::zest kota::deco)
    clice_precompile_headers(unit_tests)
    target_compile_definitions(unit_tests PRIVATE
        CLICE_TESTS_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    )
    # The generated/ include dir propagates through clice::core, but the
    # generation order does not — depend explicitly so a test including
    # version.h cannot race the generator on a clean parallel build.
    add_dependencies(unit_tests generate_version_header)
endif()

if(CLICE_ENABLE_BENCHMARK)
    foreach(benchmark scan_benchmark pipeline_benchmark pch_chain_benchmark index_stats_benchmark)
        add_executable(${benchmark}
            "${PROJECT_SOURCE_DIR}/benchmarks/${benchmark}.cpp"
        )
        target_include_directories(${benchmark} PRIVATE
            "${PROJECT_SOURCE_DIR}/src"
        )
        target_link_libraries(${benchmark} PRIVATE clice::server kota::deco)
        # The benchmarks locate lib/clang next to their binary; an explicit
        # `ninja <benchmark>` must not race or skip the resource copy.
        add_dependencies(${benchmark} copy_clang_resource)
    endforeach()
endif()

include("${PROJECT_SOURCE_DIR}/cmake/release.cmake")
