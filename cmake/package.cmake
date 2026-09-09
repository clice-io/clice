include_guard()

# CPM keeps every download in CPM_SOURCE_CACHE, so every build directory of
# every checkout reuses the dependency checkouts and the extracted LLVM
# archive instead of fetching gigabytes again. The default is the user's
# cache directory — the source tree may be read-only, and a build tree is
# too short-lived; the CPM_SOURCE_CACHE environment variable and a -D on the
# command line win over it.
if(DEFINED ENV{CPM_SOURCE_CACHE})
    set(_cpm_cache_default "$ENV{CPM_SOURCE_CACHE}")
elseif(WIN32 AND DEFINED ENV{LOCALAPPDATA})
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/clice/cpm" _cpm_cache_default)
elseif(DEFINED ENV{XDG_CACHE_HOME})
    set(_cpm_cache_default "$ENV{XDG_CACHE_HOME}/clice/cpm")
elseif(DEFINED ENV{HOME})
    set(_cpm_cache_default "$ENV{HOME}/.cache/clice/cpm")
else()
    set(_cpm_cache_default "${CMAKE_BINARY_DIR}/cpm-cache")
endif()
set(CPM_SOURCE_CACHE "${_cpm_cache_default}" CACHE PATH "Directory to download CPM dependencies")
include(${CMAKE_CURRENT_LIST_DIR}/CPM.cmake)

# Without the cache CPM falls back to FetchContent under the build tree; keep
# those checkouts from fetching on every reconfigure.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

include(${CMAKE_CURRENT_LIST_DIR}/llvm.cmake)
setup_llvm("22.1.8")

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
CPMAddPackage(
    NAME kotatsu
    GIT_REPOSITORY https://github.com/clice-io/kotatsu
    GIT_TAG 0cbb8d4f19a87fb0737cacf5306d351dd3fd45bd
)

set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
CPMAddPackage(
    NAME spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
    GIT_SHALLOW TRUE
)

set(ENABLE_ROARING_TESTS OFF CACHE INTERNAL "" FORCE)
set(ENABLE_ROARING_MICROBENCHMARKS OFF CACHE INTERNAL "" FORCE)
CPMAddPackage(
    NAME croaring
    GIT_REPOSITORY https://github.com/RoaringBitmap/CRoaring.git
    GIT_TAG v4.4.2
    GIT_SHALLOW TRUE
)

# lmdb — index blob database backend (index::BlobDatabase). Upstream ships
# no CMake; the two-file static library is defined below. Pinned to the
# 0.9 stable line.
CPMAddPackage(
    NAME lmdb
    GIT_REPOSITORY https://github.com/LMDB/lmdb.git
    GIT_TAG LMDB_0.9.31
    GIT_SHALLOW TRUE
    DOWNLOAD_ONLY YES
)
if(NOT lmdb_SOURCE_DIR)
    message(FATAL_ERROR "lmdb is built from its sources here; a local lmdb package cannot stand in for them.")
endif()

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
