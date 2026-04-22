include_guard()

include(${CMAKE_CURRENT_LIST_DIR}/llvm.cmake)
setup_llvm("21.1.8")

# install dependencies
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
    GIT_SHALLOW TRUE
)
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)

# croaring
FetchContent_Declare(
    croaring
    GIT_REPOSITORY https://github.com/RoaringBitmap/CRoaring.git
    GIT_TAG v4.4.2
    GIT_SHALLOW TRUE
)
set(ENABLE_ROARING_TESTS OFF CACHE INTERNAL "" FORCE)
set(ENABLE_ROARING_MICROBENCHMARKS OFF CACHE INTERNAL "" FORCE)

# flatbuffers
FetchContent_Declare(
    flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG v25.9.23
    GIT_SHALLOW TRUE
)
set(FLATBUFFERS_BUILD_GRPC OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    kotatsu
    GIT_REPOSITORY https://github.com/clice-io/kotatsu
    GIT_TAG 048f23f0d786
)

set(KOTA_ENABLE_ZEST ON)
set(KOTA_ENABLE_TEST OFF)
set(KOTA_CODEC_ENABLE_SIMDJSON ON)
set(KOTA_CODEC_ENABLE_YYJSON ON)
set(KOTA_CODEC_ENABLE_TOML ON)
set(KOTA_ENABLE_EXCEPTIONS OFF)
set(KOTA_ENABLE_RTTI OFF)

FetchContent_MakeAvailable(kotatsu spdlog croaring flatbuffers)
