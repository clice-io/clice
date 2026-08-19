include_guard()

include(${CMAKE_CURRENT_LIST_DIR}/llvm.cmake)
setup_llvm("22.1.8")

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
    GIT_TAG 5232e67c28fee3a85ad01341a675dd4b78b122b1
)

set(KOTA_ENABLE_ZEST ON)
set(KOTA_ENABLE_TEST OFF)
set(KOTA_CODEC_ENABLE_SIMDJSON ON)
set(KOTA_CODEC_ENABLE_YYJSON ON)
set(KOTA_CODEC_ENABLE_TOML ON)
# kotatsu fetches the flatbuffers runtime (v25.2.10) for its codec and links it
# into anything that uses kota::codec; index serialization rides on that copy.
set(KOTA_CODEC_ENABLE_FLATBUFFERS ON)
set(KOTA_ENABLE_EXCEPTIONS OFF)
set(KOTA_ENABLE_RTTI OFF)

# lmdb — index blob database backend (index::BlobDatabase). Upstream ships
# no CMake; the two-file static library is defined below. Pinned to the
# 0.9 stable line.
FetchContent_Declare(
    lmdb
    GIT_REPOSITORY https://github.com/LMDB/lmdb.git
    GIT_TAG LMDB_0.9.31
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(kotatsu spdlog croaring lmdb)

add_library(lmdb STATIC
    ${lmdb_SOURCE_DIR}/libraries/liblmdb/mdb.c
    ${lmdb_SOURCE_DIR}/libraries/liblmdb/midl.c)
target_include_directories(lmdb SYSTEM PUBLIC ${lmdb_SOURCE_DIR}/libraries/liblmdb)
# Third-party C, not held to the project's warning set.
if(MSVC)
    target_compile_options(lmdb PRIVATE /w)
else()
    target_compile_options(lmdb PRIVATE -w)
endif()
find_package(Threads REQUIRED)
target_link_libraries(lmdb PUBLIC Threads::Threads)
