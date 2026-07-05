#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace clice {

/// cache.json slice entries for context-domain state. The structs mirror the
/// on-disk JSON layout field for field — changing them changes the format.

struct CacheModeEntry {
    std::uint32_t file;          // index into the cache path table
    std::uint32_t mode;          // HeaderMode
    std::uint64_t content_hash;  // header contents the verdict was scored on
};

struct CacheContextEntry {
    std::uint32_t file;  // index into the cache path table
    std::uint32_t host;  // index into the cache path table; ~0u = none
    std::uint32_t occurrence;
    std::string command_hash;
};

struct CacheArtifactEntry {
    std::uint32_t file;  // index into the cache path table
    std::uint32_t host;  // index into the cache path table
};

}  // namespace clice
