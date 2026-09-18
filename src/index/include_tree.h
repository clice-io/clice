#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "clang/Basic/SourceLocation.h"

namespace clice {

class CompilationUnitRef;

}

namespace clice::index {

/// The parent of a node whose directive sits in the TU's own file, and
/// node_of's answer for a file entered without a directive: the TU root is
/// not itself a node.
constexpr inline std::uint32_t no_node = ~0u;

/// One include directive of an include tree: which file it names, in
/// which file it sits (the parent node's file at `line`), and whether the
/// preprocessor entered the file there. Multiple entries of one file
/// (headers without guards) are distinct nodes; so is a directive whose
/// target was skipped (guarded or `#pragma once`, entered earlier) — an
/// edge of the document with no file entry behind it. The same nodes
/// travel in the TU envelope and persist in the manifest; only `file`
/// changes meaning at merge: a TU-local path id on the wire, a
/// FileVersion id (VersionID::raw) in a manifest.
struct IncludeNode {
    std::uint32_t file = 0;

    /// Index of the including node.
    std::uint32_t parent = no_node;

    /// 1-based line of the include directive in the parent.
    std::uint32_t line = 0;

    /// The directive did not enter the file: nothing hangs off this node
    /// and node_of never answers it.
    bool skipped = false;

    friend bool operator==(const IncludeNode&, const IncludeNode&) = default;
};

/// The include tree of one compilation as the envelope carries it. Path
/// ids are TU-local; the main file is always the last path, a convention
/// serialization and the indexer rely on.
struct IncludeTree {
    /// If a header file doesn't have a #pragma once or guard macro, each
    /// inclusion of it introduces a new node; the path is stored once.
    std::vector<std::string> paths;

    /// Parallel to `paths`: hash of the bytes this compilation actually
    /// consumed per file (0 = the buffer was unavailable for hashing).
    /// Freshness baselines built from these describe what the index rows
    /// were built from, never a later disk state.
    std::vector<std::uint64_t> path_hashes;

    /// Every include edge of the parse. Besides backing `path_id` lookups,
    /// this doubles as the TU's dependency set for shard freshness checks,
    /// so it keeps every edge even when no index row lands in the included
    /// file.
    std::vector<IncludeNode> nodes;

    /// Build-time only: each FileID (one header context) to the node that
    /// entered it, no_node for a file entered without an include directive.
    /// FileIDs mean nothing outside the compilation.
    llvm::DenseMap<clang::FileID, std::uint32_t> file_nodes;

    /// Build the tree for `unit`. The nodes cover the union of every file
    /// included in this parse (from the replayed directives) and
    /// `indexed_fids` — the files the index actually recorded rows for.
    /// The latter can lie outside the directives universe: with a preamble
    /// PCH the preamble headers' FileIDs are loaded from the AST file and
    /// this parse's preprocessor callbacks never see them. Their include
    /// chains are recovered through the SourceManager, which preserves
    /// include locations across PCH boundaries.
    static IncludeTree from(CompilationUnitRef unit,
                            llvm::ArrayRef<clang::FileID> indexed_fids = {});

    /// The node that entered `fid`, or no_node for a file entered without an
    /// include directive (the main file, synthetic buffers) — and,
    /// defensively, for a fid missing from the table: the indexer must
    /// never crash on unexpected input.
    std::uint32_t node_of(clang::FileID fid) const;

    /// The path of the file `fid` refers to. A fid without a node resolves
    /// to the main file's path.
    ///
    /// FIXME: Synthetic buffers (<built-in>, <command line>) also resolve
    /// to the main file here. Once macro definitions from those buffers
    /// are indexed (see add_macro in directive.cpp), give them named path
    /// entries so consumers can route them to a preview document instead
    /// of misattributing them.
    std::uint32_t path_id(clang::FileID fid) const {
        auto node = node_of(fid);
        if(node != no_node) {
            return nodes[node].file;
        }
        return paths.size() - 1;
    }
};

}  // namespace clice::index
