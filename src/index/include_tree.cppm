module;

#include "support/prelude.h"

module clice:index.include_tree;

import :compile.compilation;
import :index.symbol;

namespace clice::index {

/// The include tree of one compilation as the TU index carries it. Path
/// ids are TU-local; the focus file is always the last path, a convention
/// serialization and the indexer rely on.
struct IncludeTree {
    /// If a header file doesn't have a #pragma once or guard macro, each
    /// inclusion of it introduces a new node; the path is stored once.
    std::vector<std::string> paths;

    /// Parallel to `paths`: hash of the bytes this compilation actually
    /// consumed per file (0 = the buffer was unavailable for hashing).
    /// Freshness baselines built from these describe what the index was
    /// built from, never a later disk state.
    std::vector<std::uint64_t> content_hashes;

    /// Every include edge of the parse. Besides backing `path_id` lookups,
    /// these are the TU's include dependencies for shard freshness checks,
    /// so it keeps every edge even when no index entry ends up in the
    /// included file.
    std::vector<IncludeNode> nodes;

    /// Build-time only: each FileID (one header context) to the node that
    /// entered it, no_node for a file entered without an include directive.
    /// FileIDs mean nothing outside the compilation.
    llvm::DenseMap<clang::FileID, std::uint32_t> file_nodes;

    /// Build the tree for `unit`. The nodes cover the union of every file
    /// included in this parse (from the replayed directives) and
    /// `indexed_fids` — the files the index actually recorded entries for.
    /// The latter can lie outside the directives universe: with a preamble
    /// PCH the preamble headers' FileIDs are loaded from the AST file and
    /// this parse's preprocessor callbacks never see them. Their include
    /// chains are recovered through the SourceManager, which preserves
    /// include locations across PCH boundaries.
    static IncludeTree from(CompilationUnit& unit, llvm::ArrayRef<clang::FileID> indexed_fids = {});

    /// The node that entered `fid`, or no_node for a file entered without an
    /// include directive (the focus file, synthetic buffers) — and,
    /// defensively, for a fid missing from the table: the indexer must
    /// never crash on unexpected input.
    std::uint32_t node_of(clang::FileID fid) const;

    /// The path of the file `fid` refers to. A fid without a node resolves
    /// to the focus file's path.
    ///
    /// FIXME: Synthetic buffers (<built-in>, <command line>) also resolve
    /// to the focus file here. Once macro definitions from those buffers
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
