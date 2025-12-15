include_guard()

include(${CMAKE_CURRENT_LIST_DIR}/llvm.cmake)
setup_llvm("21.1.4+r1")

# install dependencies
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

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
    GIT_SHALLOW    TRUE

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
    GIT_TAG        v1.15.3
    GIT_SHALLOW    TRUE
)

# tomlplusplus
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)

# croaring
FetchContent_Declare(
    croaring
    GIT_REPOSITORY https://github.com/RoaringBitmap/CRoaring.git
    GIT_TAG        v4.4.2
    GIT_SHALLOW    TRUE
)
set(ENABLE_ROARING_TESTS OFF CACHE INTERNAL "" FORCE)
set(ENABLE_ROARING_MICROBENCHMARKS OFF CACHE INTERNAL "" FORCE)

# flatbuffers
FetchContent_Declare(
    flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v25.9.23
    GIT_SHALLOW    TRUE
)
set(FLATBUFFERS_BUILD_GRPC OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)

# cpptrace
FetchContent_Declare(
    cpptrace
    GIT_REPOSITORY https://github.com/jeremy-rifkin/cpptrace.git
    GIT_TAG        v1.0.4
    GIT_SHALLOW    TRUE
)
set(CPPTRACE_DISABLE_CXX_20_MODULES ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(libuv spdlog tomlplusplus croaring flatbuffers cpptrace)

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
