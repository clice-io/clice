#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "syntax/token.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"

namespace clice {

class CompilationUnitRef;

}

namespace clice::index {

struct IncludeLocation {
    /// The file path of the include directive.
    std::uint32_t path_id = -1;

    /// The line number of the include directive, 1-based.
    std::uint32_t line = -1;

    /// The include location that introduces this file.
    std::uint32_t include = -1;

    friend bool operator==(const IncludeLocation&, const IncludeLocation&) = default;
};

struct IncludeGraph {
    /// If a header file doesn't have a #pragma once or guard macro,
    /// each inclusion of it will introduce a new header context, we
    /// don't want to save its path repeatedly, so cache it here.
    std::vector<std::string> paths;

    /// All include locations in this tu. Besides backing `path_id`
    /// lookups, this doubles as the TU's dependency set for shard
    /// freshness checks, so it must keep every include edge of the
    /// parse even when no index row lands in the included file.
    std::vector<IncludeLocation> locations;

    /// Parallel to `paths`: hash of the bytes this compilation actually
    /// consumed per file (0 = the buffer was unavailable for hashing).
    /// Freshness baselines built from these describe what the index rows
    /// were built from, never a later disk state.
    std::vector<std::uint64_t> path_hashes;

    /// Each `FileID` represents a new header context and is introduced
    /// by a new include directive. So a include directive is a new header
    /// context. A map between FileID and its include location.
    llvm::DenseMap<clang::FileID, std::uint32_t> file_table;

    /// Build the graph for `unit`. The file table covers the union of
    /// every file included in this parse (from the replayed directives)
    /// and `indexed_fids` — the files the index actually recorded rows
    /// for. The latter can lie outside the directives universe: with a
    /// preamble PCH the preamble headers' FileIDs are loaded from the
    /// AST file and this parse's preprocessor callbacks never see them.
    /// Their include chains are recovered through the SourceManager,
    /// which preserves include locations across PCH boundaries.
    ///
    /// The interested file's path is always the last entry of `paths`;
    /// serialization and the indexer rely on this convention.
    static IncludeGraph from(CompilationUnitRef unit,
                             llvm::ArrayRef<clang::FileID> indexed_fids = {});

    llvm::StringRef path(std::uint32_t path_ref) const {
        assert(path_ref < paths.size());
        return paths[path_ref];
    }

    /// The include location introducing `fid`, or -1 for a file entered
    /// without an include directive (the interested file, synthetic
    /// buffers) — and, defensively, for a fid missing from the table:
    /// the indexer must never crash on unexpected input.
    std::uint32_t include_location_id(clang::FileID fid) const;

    /// The path of the file `fid` refers to. An fid without an include
    /// location resolves to the interested file's path.
    ///
    /// FIXME: Synthetic buffers (<built-in>, <command line>) also resolve
    /// to the interested file here. Once macro definitions from those
    /// buffers are indexed (see add_macro in directive.cpp), give them
    /// named path entries so consumers can route them to a preview
    /// document instead of misattributing them.
    std::uint32_t path_id(clang::FileID fid) const {
        auto include = include_location_id(fid);
        if(include != -1) {
            return locations[include].path_id;
        } else {
            return paths.size() - 1;
        }
    }
};

}  // namespace clice::index
