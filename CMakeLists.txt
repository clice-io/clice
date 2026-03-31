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
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,/OPT:NOICF")
        string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,/OPT:NOICF")
    endif()
endif()

include("${PROJECT_SOURCE_DIR}/cmake/package.cmake")

# Project-specific options (not applied to third-party deps).
add_library(clice_options INTERFACE)

if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
         CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(clice_options INTERFACE /GR- /EHsc- /Zc:preprocessor)
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

add_custom_command(
    OUTPUT "${GENERATED_HEADER}"
    COMMAND $<TARGET_FILE:flatc> --cpp -o "${PROJECT_BINARY_DIR}/generated" "${FBS_SCHEMA_FILE}"
    DEPENDS "${FBS_SCHEMA_FILE}"
    COMMENT "Generating C++ header from ${FBS_SCHEMA_FILE}"
)

add_custom_target(generate_flatbuffers_schema DEPENDS "${GENERATED_HEADER}")

# Temporary migration-only build graph.
add_library(clice-core STATIC
    "${PROJECT_SOURCE_DIR}/src/command/argument_parser.cpp"
    "${PROJECT_SOURCE_DIR}/src/command/command.cpp"
    "${PROJECT_SOURCE_DIR}/src/command/search_config.cpp"
    "${PROJECT_SOURCE_DIR}/src/command/toolchain.cpp"
    "${PROJECT_SOURCE_DIR}/src/compile/compilation.cpp"
    "${PROJECT_SOURCE_DIR}/src/compile/compilation_unit.cpp"
    "${PROJECT_SOURCE_DIR}/src/compile/diagnostic.cpp"
    "${PROJECT_SOURCE_DIR}/src/compile/directive.cpp"
    "${PROJECT_SOURCE_DIR}/src/compile/tidy.cpp"
    "${PROJECT_SOURCE_DIR}/src/support/doxygen.cpp"
    "${PROJECT_SOURCE_DIR}/src/support/structed_text.cpp"
    "${PROJECT_SOURCE_DIR}/src/support/fuzzy_matcher.cpp"
    "${PROJECT_SOURCE_DIR}/src/support/glob_pattern.cpp"
    "${PROJECT_SOURCE_DIR}/src/support/logging.cpp"
    "${PROJECT_SOURCE_DIR}/src/syntax/lexer.cpp"
    "${PROJECT_SOURCE_DIR}/src/syntax/scan.cpp"
    "${PROJECT_SOURCE_DIR}/src/syntax/include_resolver.cpp"
    "${PROJECT_SOURCE_DIR}/src/syntax/dependency_graph.cpp"
    "${PROJECT_SOURCE_DIR}/src/feature/semantic_tokens.cpp"
    "${PROJECT_SOURCE_DIR}/src/feature/document_links.cpp"
    "${PROJECT_SOURCE_DIR}/src/feature/document_symbols.cpp"
    "${PROJECT_SOURCE_DIR}/src/feature/folding_ranges.cpp"
    "${PROJECT_SOURCE_DIR}/src/feature/code_completion.cpp"
    "${PROJECT_SOURCE_DIR}/src/feature/hover.cpp"
    "${PROJECT_SOURCE_DIR}/src/feature/inlay_hints.cpp"
    "${PROJECT_SOURCE_DIR}/src/feature/signature_help.cpp"
    "${PROJECT_SOURCE_DIR}/src/feature/formatting.cpp"
    "${PROJECT_SOURCE_DIR}/src/feature/diagnostics.cpp"
    "${PROJECT_SOURCE_DIR}/src/semantic/resolver.cpp"
    "${PROJECT_SOURCE_DIR}/src/semantic/symbol_kind.cpp"
    "${PROJECT_SOURCE_DIR}/src/semantic/ast_utility.cpp"
    "${PROJECT_SOURCE_DIR}/src/semantic/selection.cpp"
    "${PROJECT_SOURCE_DIR}/src/index/include_graph.cpp"
    "${PROJECT_SOURCE_DIR}/src/index/tu_index.cpp"
    "${PROJECT_SOURCE_DIR}/src/index/usr_generation.cpp"
    "${PROJECT_SOURCE_DIR}/src/index/project_index.cpp"
    "${PROJECT_SOURCE_DIR}/src/index/merged_index.cpp"
    "${PROJECT_SOURCE_DIR}/src/server/stateless_worker.cpp"
    "${PROJECT_SOURCE_DIR}/src/server/stateful_worker.cpp"
    "${PROJECT_SOURCE_DIR}/src/server/worker_pool.cpp"
    "${PROJECT_SOURCE_DIR}/src/server/compile_graph.cpp"
    "${PROJECT_SOURCE_DIR}/src/server/master_server.cpp"
    "${PROJECT_SOURCE_DIR}/src/server/config.cpp"
)
add_library(clice::core ALIAS clice-core)
add_dependencies(clice-core generate_flatbuffers_schema)

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
    eventide::ipc::lsp
    eventide::serde::toml
    simdjson::simdjson
)

add_executable(clice "${PROJECT_SOURCE_DIR}/src/clice.cc")
target_link_libraries(clice PRIVATE clice::core eventide::deco)
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
    target_link_libraries(unit_tests PRIVATE clice::core eventide::zest eventide::deco)
endif()

if(CLICE_ENABLE_BENCHMARK)
    add_executable(scan_benchmark
        "${PROJECT_SOURCE_DIR}/benchmarks/scan_benchmark.cpp"
    )
    target_include_directories(scan_benchmark PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(scan_benchmark PRIVATE clice::core eventide::deco)
endif()

if(CLICE_RELEASE)
    set(CLICE_PACK_DIR "${PROJECT_BINARY_DIR}/pack")
    set(CLICE_SYMBOL_DIR "${PROJECT_BINARY_DIR}/pack-symbol")

    if(WIN32)
        set(CLICE_ARCHIVE_EXT ".zip")
    else()
        set(CLICE_ARCHIVE_EXT ".tar.gz")
    endif()

    if(WIN32)
        set(CLICE_EXE_NAME "clice.exe")
        set(CLICE_SYMBOL_NAME "clice.pdb")
    elseif(APPLE)
        set(CLICE_EXE_NAME "clice")
        set(CLICE_SYMBOL_NAME "clice.dSYM")
    else()
        set(CLICE_EXE_NAME "clice")
        set(CLICE_SYMBOL_NAME "clice.debug")
    endif()

    if(WIN32)
        add_custom_target(clice-strip ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}"
            DEPENDS clice
            COMMENT "Collecting PDB for clice"
        )
    elseif(APPLE)
        add_custom_target(clice-strip ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}"
            COMMAND dsymutil "$<TARGET_FILE:clice>" -o "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}"
            COMMAND strip -x "$<TARGET_FILE:clice>"
            DEPENDS clice
            COMMENT "Extracting dSYM and stripping clice"
        )
    else()
        add_custom_target(clice-strip ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}"
            COMMAND ${CMAKE_OBJCOPY} --only-keep-debug "$<TARGET_FILE:clice>" "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}"
            COMMAND ${CMAKE_STRIP} --strip-debug --strip-unneeded "$<TARGET_FILE:clice>"
            COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink="${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}" "$<TARGET_FILE:clice>"
            DEPENDS clice
            COMMENT "Extracting debug symbols and stripping clice"
        )
    endif()

    add_custom_target(clice-pack ALL
        DEPENDS clice-strip copy_clang_resource
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${CLICE_PACK_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_PACK_DIR}/clice/bin"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:clice>" "${CLICE_PACK_DIR}/clice/bin/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${LLVM_INSTALL_PATH}/lib/clang" "${CLICE_PACK_DIR}/clice/lib/clang"
        COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_SOURCE_DIR}/docs/clice.toml" "${CLICE_PACK_DIR}/clice/"
        COMMAND ${CMAKE_COMMAND}
            -DARCHIVE_DIR="${CLICE_PACK_DIR}/clice"
            -DOUTPUT="${PROJECT_BINARY_DIR}/clice${CLICE_ARCHIVE_EXT}"
            -DWORK_DIR="${CLICE_PACK_DIR}"
            -P "${PROJECT_SOURCE_DIR}/cmake/archive.cmake"
        COMMENT "Packaging clice distribution"
    )

    add_custom_target(clice-pack-symbol ALL
        DEPENDS clice-strip
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${CLICE_SYMBOL_DIR}/pack"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLICE_SYMBOL_DIR}/pack"
        COMMAND ${CMAKE_COMMAND} -E copy "${CLICE_SYMBOL_DIR}/${CLICE_SYMBOL_NAME}" "${CLICE_SYMBOL_DIR}/pack/"
        COMMAND ${CMAKE_COMMAND}
            -DARCHIVE_DIR="${CLICE_SYMBOL_DIR}/pack"
            -DOUTPUT="${PROJECT_BINARY_DIR}/clice-symbol${CLICE_ARCHIVE_EXT}"
            -DWORK_DIR="${CLICE_SYMBOL_DIR}/pack"
            -P "${PROJECT_SOURCE_DIR}/cmake/archive.cmake"
        COMMENT "Packaging clice debug symbols"
    )
endif()
