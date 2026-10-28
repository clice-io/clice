module;

#include "support/prelude.h"

module clice:feature.index_entries;

import :index.shard;
import :index.symbol;
import :syntax.token;

namespace clice::feature {

/// One Decl/Def relation of the document: the name-token anchor, the
/// declaration's full extent (invalid when the index recorded none — a
/// cross-file or invalid source range) and the symbol it belongs to.
struct IndexDecl {
    LocalSourceRange range;
    LocalSourceRange extent;
    index::SymbolHash symbol = 0;
    bool definition = false;
};

/// The entries of one document the features computed from the index read,
/// extracted from its serving shard: every occurrence, the reference
/// relations of names nested in another's written name (the class in
/// `~Foo`, which owns no occurrence there) as occurrences of their own, and
/// the Decl/Def relations as decl entries. Those features serve open files
/// that have no AST yet (see Requests' routing rules) and take these entries
/// and narrow resolvers, never index storage. Each one's output is an honest
/// subset of its AST twin's: missing pieces (Sema modifiers, outline detail,
/// statement folds, ...) are pinned by the index/read_only cases.
struct IndexEntries {
    std::vector<index::Occurrence> occurrences;
    std::vector<IndexDecl> decls;

    /// The explicit instantiations, as decl entries of what they
    /// instantiate (RelationKind::Instantiation).
    std::vector<IndexDecl> instantiations;

    /// The names written to (RelationKind::Write).
    std::vector<index::Occurrence> writes;
};

auto extract_index_entries(const index::Shard& shard) -> IndexEntries;

using IndexSymbolResolver = llvm::function_ref<std::optional<index::SymbolInfo>(index::SymbolHash)>;

}  // namespace clice::feature
