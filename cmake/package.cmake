include_guard()

include(${CMAKE_CURRENT_LIST_DIR}/llvm.cmake)
setup_llvm("21.1.8+r2")

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

FetchContent_Declare(
    kotatsu
    GIT_REPOSITORY https://github.com/clice-io/kotatsu
    GIT_TAG c516e3ae0ca3c7d7fb35fdcfdc7c6a111adef764
)

set(KOTA_ENABLE_ZEST ON)
set(KOTA_ENABLE_TEST OFF)
set(KOTA_CODEC_ENABLE_SIMDJSON ON)
set(KOTA_CODEC_ENABLE_YYJSON ON)
set(KOTA_CODEC_ENABLE_TOML ON)
# kotatsu already fetches flatbuffers (v25.2.10) for its own codec and links the
# runtime lib into anything that uses kota::codec, so clice rides on that copy
# instead of fetching a second one. kotatsu does not build flatc, though, so the
# schema compiler comes from pixi instead (see CMakeLists.txt).
set(KOTA_CODEC_ENABLE_FLATBUFFERS ON)
set(KOTA_ENABLE_EXCEPTIONS OFF)
set(KOTA_ENABLE_RTTI OFF)

FetchContent_MakeAvailable(kotatsu spdlog croaring)
