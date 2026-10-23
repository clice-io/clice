module;

#include "support/prelude.h"

module clice:feature.workspace_symbols;

import :index.peer_queries;
import :index.site;
import :index.symbol;
import :vfs.path;

namespace clice::feature {

/// `container` is the qualified name of the symbol's parent, empty at the
/// translation unit. A function is named with its signature, which tells
/// its overloads apart.
protocol::SymbolInformation symbol_information(const index::SymbolInfo& symbol,
                                               const index::Site& site,
                                               llvm::StringRef container);

/// The most workspace/symbol answers, from one project or all.
constexpr inline std::size_t workspace_symbol_limit = 100;

struct WorkspaceSymbol {
    protocol::SymbolInformation info;

    /// Its file's name relative to the project (path::portable) and the
    /// hash of the text indexed, which another checkout of the same code
    /// shares.
    std::string file;
    std::uint64_t content = 0;
};

/// The project's symbols matching `text` (index::SymbolQuery, its paths
/// relative to `root`), best first, at most workspace_symbol_limit; an
/// open file's only from the project serving its buffer.
std::vector<WorkspaceSymbol> workspace_symbols(const index::PeerQueries& queries,
                                               llvm::StringRef text,
                                               const Spelling& root);

/// Every project's list (workspace_symbols) merged rank by rank, in the
/// order given: a symbol several projects see at one place lists once, the
/// first list's, and so does the copy another checkout of the same code
/// holds at the same place in the same text.
std::vector<protocol::SymbolInformation>
    merge_workspace_symbols(llvm::MutableArrayRef<std::vector<WorkspaceSymbol>> ranked);

}  // namespace clice::feature
