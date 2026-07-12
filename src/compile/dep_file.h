#pragma once

#include <cstdint>
#include <string>

namespace clice {

/// One compile input paired with the hash of the bytes the compiler actually
/// consumed for it — taken from the compiler's own in-memory buffers at build
/// time, never from a later disk read. A later disk read could describe
/// content the build never saw (the file may change while the build runs),
/// which would poison every staleness check built on it.
///
/// hash == 0 means the consumed bytes were not available for hashing; the
/// staleness check treats such an entry conservatively (rebuild once).
struct DepFile {
    std::string path;
    std::uint64_t hash = 0;
};

}  // namespace clice
