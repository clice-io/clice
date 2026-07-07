#pragma once

#include <cstdint>
#include <string>

namespace clice {

/// A file a compilation consumed, together with the xxh3_64 hash of the
/// content the compiler actually read. The hash is computed by the worker at
/// the end of the compilation from the SourceManager-resident buffer (module
/// interface sources, which are consumed indirectly through their PCM, are
/// hashed from disk instead). The master adopts these hashes verbatim for its
/// staleness snapshots, so "fresh" always means "the artifact was built from
/// exactly this content" — never "the disk happened to look like this when
/// the result arrived".
///
/// A hash of 0 means the content could not be read (same sentinel as
/// hash_file); staleness checks treat it conservatively.
struct HashedDep {
    std::string path;
    std::uint64_t hash = 0;

    friend bool operator==(const HashedDep&, const HashedDep&) = default;
};

}  // namespace clice
