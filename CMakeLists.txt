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

set(FBS_SCHEMA_FILE "${CMAKE_CURRENT_SOURCE_DIR}/include/Index/schema.fbs")
set(GENERATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/schema_generated.h")

add_custom_command(
    OUTPUT ${GENERATED_HEADER}
    COMMAND $<TARGET_FILE:flatc> --cpp -o ${CMAKE_CURRENT_BINARY_DIR}/generated ${FBS_SCHEMA_FILE}
    DEPENDS ${FBS_SCHEMA_FILE}
    COMMENT "Generating C++ header from ${FBS_SCHEMA_FILE}"
)

add_custom_target(
    generate_flatbuffers_schema
    DEPENDS ${GENERATED_HEADER}
)

set(CONFIG_SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config/clang-tidy-config.h")
set(CONFIG_GENERATED_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/clang-tidy-config.h")

add_custom_command(
    OUTPUT ${CONFIG_GENERATED_FILE}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CONFIG_SOURCE_FILE} ${CONFIG_GENERATED_FILE}
    DEPENDS ${CONFIG_SOURCE_FILE}
    COMMENT "Generating C++ header from ${CONFIG_SOURCE_FILE}"
)

add_custom_target(
    generate_config
    DEPENDS ${CONFIG_GENERATED_FILE}
)

file(GLOB_RECURSE CLICE_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/AST/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/Async/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/Basic/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/Compiler/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/Index/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/Feature/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/Server/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/Support/*.cpp"
)
add_library(clice-core STATIC "${CLICE_SOURCES}")
add_dependencies(clice-core generate_flatbuffers_schema generate_config)

target_include_directories(clice-core PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(clice-core PUBLIC
    clice_options
    libuv::libuv
    spdlog::spdlog
    tomlplusplus::tomlplusplus
    roaring::roaring
    flatbuffers
    llvm-libs
)

add_executable(clice "${PROJECT_SOURCE_DIR}/bin/clice.cc")
target_link_libraries(clice PRIVATE clice-core)
install(TARGETS clice RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

message(STATUS "Copying resource directory for development build")
file(
    COPY "${LLVM_INSTALL_PATH}/lib/clang"
    DESTINATION "${PROJECT_BINARY_DIR}/lib"
)
install(
    DIRECTORY "${LLVM_INSTALL_PATH}/lib/clang"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

if(CLICE_ENABLE_TEST)
    file(GLOB_RECURSE CLICE_TEST_SOURCES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tests/unit/*/*.cpp")
    add_executable(unit_tests
        "${CLICE_TEST_SOURCES}"
        "${PROJECT_SOURCE_DIR}/bin/unit_tests.cc"
    )
    target_include_directories(unit_tests PUBLIC "${PROJECT_SOURCE_DIR}")
    target_link_libraries(unit_tests PRIVATE clice-core cpptrace::cpptrace)
endif()
