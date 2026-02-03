include_guard()

include(${CMAKE_CURRENT_LIST_DIR}/llvm.cmake)
setup_llvm("21.1.4+r1")

# install dependencies
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

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

FetchContent_Declare(
    eventide
    GIT_REPOSITORY https://github.com/clice-io/eventide
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)
set(EVENTIDE_ENABLE_ZEST ON)
set(EVENTIDE_ENABLE_TEST OFF)

FetchContent_MakeAvailable(eventide spdlog tomlplusplus croaring flatbuffers)

target_compile_definitions(spdlog PUBLIC
    SPDLOG_USE_STD_FORMAT=1
    SPDLOG_NO_EXCEPTIONS=1
)
