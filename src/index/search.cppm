module;

#include "modules/prelude.h"

/// Symbols by name: the ranked search over the search index, what
/// unqualified completion may offer, and the symbols a locator names.

module clice:index.search;

import :index.query;
import :index.symbol;
import :index.symbol_query;

namespace clice::index {

/// The symbols a name query (SymbolQuery) matches, best
/// first, at most `limit`: the search index's hits, the symbols merged
/// since it was built and the open sessions' own symbols — scanned
/// directly — under one ranking, each with its canonical site. A query
/// naming files also finds the symbols only those files see — locals,
/// parameters, internal-linkage entities — among what each declares.
/// A symbol no source places is left out. Empty for a query by id or
/// place.
std::vector<IndexQuery::Located> search(const IndexQuery& index, const SymbolQuery& query, std::size_t limit);

/// A name unqualified completion may offer from the index, with the
/// file of its canonical declaration: invalid for one of a preamble,
/// which the file reaches already.
struct Completable {
    SymbolInfo symbol;
    Fid file;
};

/// The names `prefix` matches that unqualified completion in `file` may
/// offer — names of namespaces and unscoped enumerators
/// (SymbolFlags::Completable) linked across files — best first, at most
/// `limit`: the project's, the open buffers' and those of `file`'s
/// preamble. Whether the limit cut them short.
struct Completables {
    std::vector<Completable> names;
    bool exhausted = true;
};

Completables completables(const IndexQuery& index, llvm::StringRef prefix, Fid file, std::size_t limit);

/// The symbols a locator query names: by id; by place — the symbol
/// under a cursor, or those defined on a line of the file's serving
/// source; or by pattern, where every symbol spelling the name exactly
/// (in any case) is the answer when there is one, and the ranked
/// matches stand as candidates otherwise. A template's name without
/// arguments names the template, not its specializations.
std::vector<IndexQuery::Located> locate(const IndexQuery& index, const SymbolQuery& query);

}  // namespace clice::index
