module;

/// Symbols by name: the ranked search over the search index, what
/// unqualified completion may offer, and the symbols a locator names.

module clice:index.search;

import :index.query;
import :index.symbol;
import :index.symbol_query;
import :support.prelude;

namespace clice::index {

/// The symbols a name query (SymbolQuery) matches, best
/// first, at most `limit`: the search index's hits, the symbols merged
/// since it was built and the open sessions' own symbols — scanned
/// directly — under one ranking, each with its canonical site. A query
/// naming files also finds the symbols only those files see — locals,
/// parameters, internal-linkage entities — among what each declares.
/// A symbol no source places is left out. Empty for a query by id or
/// place.
std::vector<IndexQuery::Located> search(const IndexQuery& index,
                                        const SymbolQuery& query,
                                        std::uint32_t limit);

/// A name completion may offer from the index, with the scopes around it,
/// outermost first, and the file of its canonical declaration: invalid for
/// one of a preamble, which the file reaches already.
struct Completable {
    SymbolInfo symbol;
    llvm::SmallVector<SymbolInfo, 4> scope;
    Fid file;
};

/// Where completion looks names up: everywhere for an unqualified name, or
/// directly in the scopes a qualifier names, `absolute` when it starts
/// with `::`.
struct CompletionScope {
    std::vector<std::string> qualifier;
    bool absolute = false;

    bool qualified() const {
        return absolute || !qualifier.empty();
    }
};

/// The names `prefix` matches that completion in `file` may offer from
/// `scope` — names of namespaces and unscoped enumerators
/// (SymbolFlags::Completable) — best first, at most `limit`: the
/// project's and the open buffers' linked across files, and every one of
/// `file`'s preamble. An empty prefix takes the names used most. After a
/// qualifier, its scopes' own names come first, then those of any other
/// scope, which a using-declaration or using-directive there may bring
/// in. Whether the limit cut each short.
struct Completables {
    std::vector<Completable> names;
    bool exhausted = true;
    bool others_exhausted = true;
};

Completables completables(const IndexQuery& index,
                          llvm::StringRef prefix,
                          const CompletionScope& scope,
                          Fid file,
                          std::uint32_t limit);

/// The symbols a locator query names: by id; by place — the symbol
/// under a cursor, or those defined on a line of the file's serving
/// source; or by pattern, where every symbol spelling the name exactly
/// (in any case) is the answer when there is one, and the ranked
/// matches stand as candidates otherwise. A template's name without
/// arguments names the template, not its specializations.
std::vector<IndexQuery::Located> locate(const IndexQuery& index, const SymbolQuery& query);

}  // namespace clice::index
