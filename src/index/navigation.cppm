module;

#include "modules/prelude.h"

/// The questions an editor asks at a cursor: where a symbol is defined and
/// declared, its references, highlights, type definition and implementations.

module clice:index.navigation;

import :index.instantiation_shard;
import :index.query;
import :index.shard;
import :index.site;
import :index.symbol;
import :semantic.symbol;

namespace clice::index {

/// How the code at a highlighted name accesses its symbol.
enum class HighlightKind : std::uint8_t {
    /// A declaration or definition, a use of a symbol nothing reads or
    /// writes, or a use only an instantiation or a macro expansion
    /// resolves, which records no access.
    Text,
    Read,
    Write,
};

struct Highlight {
    LocalSourceRange range;
    HighlightKind kind;
};

/// The relations of `symbols` in the shards of one text, and the uses of
/// them the text's instantiations and macro expansions resolve, one
/// highlight per range in range order: a declaration or definition Text; a
/// reference of a variable, field or parameter Read, a Write relation Write;
/// a reference of any other symbol, or a use only an instantiation or a
/// macro expansion resolves, Text — the strongest of them where several
/// share a range. A class's name also highlights where its constructors
/// are declared. `kind_of` is a symbol's kind, when known.
std::vector<Highlight>
    highlights(llvm::ArrayRef<const Shard*> shards,
               llvm::ArrayRef<const InstantiationShard*> instantiations,
               llvm::ArrayRef<SymbolHash> symbols,
               llvm::function_ref<std::optional<SymbolKind>(SymbolHash)> kind_of);

/// The definitions go-to-definition lists for a symbol: all of them,
/// but of a namespace, which each of its blocks defines, the canonical
/// one.
std::vector<Site> definition_sites(const IndexQuery& index, SymbolHash hash, Fid anchor);

/// Go-to-definition from a cursor: the definition sites (at its own
/// declaration, a virtual function nothing defines is defined by its
/// overrides), or —
/// standing on the definition itself, or when nothing defines the
/// symbol — the declarations (and sibling definitions) and, at a
/// specialization, the template it specializes, so definition and
/// declaration sites alternate; a namespace's blocks never do. The
/// cursor's own site drops out unless it is the only site the symbol
/// has.
std::vector<Site> definition(const IndexQuery& index, const IndexQuery::Cursor& cursor);

/// The mirror half of definition's alternation: declarations plus the
/// definition, minus the site the cursor stands on. At a function
/// defined with no declaration of its own, the declarations an edit to
/// one side may have parted from it (mismatched_declarations).
std::vector<Site> declaration(const IndexQuery& index, const IndexQuery::Cursor& cursor);

/// The references of the symbol under the cursor, weak ones included (a
/// template's call through an overload set or a dependent name, which
/// names its candidates only heuristically), optionally folding in its
/// declarations and definitions, deduplicated across the kinds — relations
/// of different kinds can share one anchor. A method's include the
/// references of the methods it overrides, a call through which may
/// dispatch to it, but not one naming that method by qualification
/// (`Base::f()`); a class's the names of its constructors.
std::vector<Site> references(const IndexQuery& index,
                             const IndexQuery::Cursor& cursor,
                             bool include_declaration);

struct PlacedHighlight {
    Site site;
    HighlightKind kind;
};

/// The highlights of the symbols under the cursor in the cursor's own
/// file, from the index serving it — a session's preamble region
/// included.
std::vector<PlacedHighlight> document_highlights(const IndexQuery& index,
                                                 const IndexQuery::Cursor& cursor);

/// Go-to-type-definition of a cursor's symbol: the declarations its
/// type names — at a name of a member of a template's specialization,
/// the type the specialization gives it (TypeDefinition relations
/// spanning the cursor in the index serving its file); at a type, the
/// type itself; elsewhere the type it is declared with (the relations
/// without a range). An alias leads on to the type it names. The
/// cursor's own site drops out unless it is the only one.
std::vector<Site> type_definition(const IndexQuery& index,
                                  const IndexQuery::Cursor& cursor,
                                  SymbolHash symbol);

/// Whether the name at the cursor may mean `symbol` without naming it
/// for certain — a dependent name, an overload set's candidate, a
/// using-declaration's target: a WeakReference relation at the
/// cursor's name in the index serving its file.
bool names_weakly(const IndexQuery& index, const IndexQuery::Cursor& cursor, SymbolHash symbol);

/// What implements the symbol, each at its canonical site: the types
/// deriving from a class-like symbol, the overrides of a function, at
/// any depth; for a function nothing overrides, its definitions.
std::vector<IndexQuery::Located> implementation(const IndexQuery& index,
                                                SymbolHash hash,
                                                Fid anchor);

}  // namespace clice::index
