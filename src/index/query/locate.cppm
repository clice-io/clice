/// Symbols by name: the ranked search over the search index, what
/// unqualified completion may offer, and the symbols a locator names.

module clice:index.query.locate;

import :index.query;
import :index.query.site;
import :index.symbol;
import :index.search.locator;
import :support.prelude;

namespace clice::index {

/// The symbols a name query (Locator) matches, best
/// first, at most `limit`: the search index's hits, the symbols merged
/// since it was built and the open sessions' own symbols — scanned
/// directly — under one ranking, each with its canonical site, a macro no
/// file defines with its predefined_site. A query naming files also finds
/// the symbols only those files see — locals, parameters, internal-linkage
/// entities — among what each declares. Any other symbol no source places
/// is left out. Empty for a query by id or place.
std::vector<IndexQuery::Located> search(const IndexQuery& index,
                                        const Locator& query,
                                        std::uint32_t limit);

/// Each of `symbols` at every definition of it when several places define
/// it — a weak function and its strong one, the `main` of each program —
/// and as it is otherwise.
std::vector<IndexQuery::Located> every_definition(const IndexQuery& index,
                                                  std::vector<IndexQuery::Located> symbols);

/// Where a macro no file defines comes from, as the site standing for it,
/// which names no file: `<command line>` (`-D`) or `<built-in>`, the
/// compiler's own. None for any other symbol.
std::optional<Site> predefined_site(const SymbolInfo& symbol);

/// The macros no file defines that a locator names, by id, by the place of
/// a use or by their exact name, each at its predefined_site: what locate()
/// leaves out, having no site in a file.
std::vector<IndexQuery::Located> predefined_macros(const IndexQuery& index, const Locator& query);

/// A name completion may offer from the index, with the scopes around it,
/// outermost first, and the file of its canonical declaration: invalid for
/// one of a preamble, which the file reaches already.
struct Completable {
    SymbolInfo symbol;
    llvm::SmallVector<SymbolInfo, 4> scope;
    Fid file;
};

/// The names `prefix` matches that completion may offer from any scope —
/// names of namespaces and unscoped enumerators, and macros
/// (SymbolFlags::Completable) — or from the namespace `scope` a qualifier
/// names (absolute: from the global scope), best first, at most `limit`
/// names, each with all its overloads: the project's and the open buffers'
/// linked across files. An empty prefix takes the names used most. Whether
/// the limit cut them short.
struct Completables {
    std::vector<Completable> names;
    bool exhausted = true;
};

Completables completables(const IndexQuery& index,
                          llvm::StringRef prefix,
                          std::uint32_t limit,
                          llvm::ArrayRef<llvm::StringRef> scope = {},
                          bool absolute = false);

/// The symbols a locator query names: by id; by place — the symbol
/// under a cursor, or those defined on a line of the file's serving
/// source; or by pattern, where every symbol spelling the name exactly
/// (in any case) is the answer when there is one, and the ranked
/// matches stand as candidates otherwise. A template's name without
/// arguments names the template, not its specializations.
std::vector<IndexQuery::Located> locate(const IndexQuery& index, const Locator& query);

/// What a message of the Objective-C method `method`'s selector may reach
/// besides it, an instance or a class one as it is: the other methods of
/// the selector, the accessors clang declares for properties among them,
/// which no code places. A message to `id` or `Class` may reach any of them.
std::vector<IndexQuery::Target> reached_by_selector(const IndexQuery& index,
                                                    const SymbolInfo& method);

/// The cursor at a place with a column, in the file's serving source;
/// none when the index of the file is stale or the place is off its text.
std::optional<IndexQuery::Cursor> cursor_at(const IndexQuery& index,
                                            const Locator::Position& place);

}  // namespace clice::index
