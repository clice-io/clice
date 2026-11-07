module clice:index.entries;

import :index.shard;
import :index.symbol;
import :support.prelude;
import :syntax.token;

namespace clice::index {

/// One Decl/Def relation of the document: the name-token anchor, the
/// declaration's full extent (invalid when the index recorded none — a
/// cross-file or invalid source range) and the symbol it belongs to.
struct DeclEntry {
    LocalSourceRange range;
    LocalSourceRange extent;
    SymbolHash symbol = 0;
    bool definition = false;
};

/// The entries of one document the features computed from the index read,
/// extracted from its serving shard: every occurrence, the reference
/// relations of names nested in another's written name (the class in
/// `~Foo`, which owns no occurrence there) as occurrences of their own, and
/// the Decl/Def relations as decl entries. Those features serve open files
/// that have no AST yet (see ServedProject::pick_route) and take these entries
/// and narrow resolvers, never index storage. Each one's answer is its AST
/// twin's or less of it (Sema modifiers, outline detail, statement folds,
/// ...): every answers case compares the two.
struct FileEntries {
    std::vector<Occurrence> occurrences;
    std::vector<DeclEntry> decls;

    /// The explicit instantiations, as decl entries of what they
    /// instantiate (RelationKind::Instantiation).
    std::vector<DeclEntry> instantiations;

    /// The names written to (RelationKind::Write).
    std::vector<Occurrence> writes;
};

auto extract_entries(const Shard& shard) -> FileEntries;

using SymbolResolver = llvm::function_ref<std::optional<SymbolInfo>(SymbolHash)>;

}  // namespace clice::index
