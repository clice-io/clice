module;

#include "support/prelude.h"

module clice:index.peer_queries;

import :index.query;
import :index.site;
import :index.symbol;
import :vfs.ids;

namespace clice::index {

/// The index queries a request reads: its own project's and those of the
/// other projects the server opened beside it, its peers. A symbol's
/// answers are also sought in the peers declaring it in the same file — an
/// application and the library beside it see each other's uses — and an
/// open file answers only through the project serving its buffer. Like
/// IndexQuery's, its answers borrow the state they were read from.
struct PeerQueries {
    const IndexQuery& own;

    llvm::function_ref<llvm::SmallVector<const IndexQuery*>()> peers;

    /// The query of the project a file open in the editor is routed to;
    /// null for a closed file.
    llvm::function_ref<const IndexQuery*(Fid)> open_in;

    /// A project asked about a symbol, and the symbol as that project
    /// names it: an id hashing its file (a macro, a file-local name) takes
    /// the file relative to the compiling project's root, so a project
    /// compiling the file from outside its root names it by another id.
    struct Asked {
        const IndexQuery* query;
        SymbolHash symbol;
    };

    /// The own query, then the peers knowing the entity `symbol` names
    /// there: declaring it in a file the own project declares it in, or in
    /// `anchor`, the file it was found in — under the same id or, at the
    /// same place in a file the own project declares it in, under another.
    llvm::SmallVector<Asked> asked(SymbolHash symbol, Fid anchor) const;

    /// A symbol's qualified name, from the own project or else the first
    /// peer that knows it: a neighbour only a peer indexes carries its id.
    /// `anchor` as for IndexQuery::symbol_info.
    std::string qualified_name(SymbolHash symbol, Fid anchor) const;

    /// Whether `from`'s shard for `file` stands among the answers of `asked`:
    /// any project's for a closed file; for an open one only the project
    /// serving its buffer — the others hold its disk shard, which the buffer
    /// superseded — unless that project is not asked.
    bool answers_for(const IndexQuery& from, Fid file, llvm::ArrayRef<Asked> asked) const;

    /// `ask`'s sites from every query asked about each of `symbols`
    /// (asked), each under its own id, in the files each may answer for
    /// (answers_for), deduplicated.
    std::vector<Site>
        gather(llvm::ArrayRef<SymbolHash> symbols,
               Fid anchor,
               llvm::function_ref<std::vector<Site>(const IndexQuery&, SymbolHash)> ask) const;
};

}  // namespace clice::index
