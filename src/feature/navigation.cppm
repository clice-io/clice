module;

/// The questions an editor asks at a cursor: where a symbol is defined and
/// declared, its references, highlights, type definition and implementations.

module clice:feature.navigation;

import :index.instantiation_shard;
import :index.peer_queries;
import :index.query;
import :index.shard;
import :index.site;
import :index.symbol;
import :semantic.symbol;
import :support.prelude;
import :syntax.position;

namespace clice::feature {

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
    highlights(llvm::ArrayRef<const index::Shard*> shards,
               llvm::ArrayRef<const index::InstantiationShard*> instantiations,
               llvm::ArrayRef<index::SymbolHash> symbols,
               llvm::function_ref<std::optional<SymbolKind>(index::SymbolHash)> kind_of);

/// The definitions go-to-definition lists for a symbol: all of them,
/// but of a namespace, which each of its blocks defines, the canonical
/// one.
std::vector<index::Site> definition_sites(const index::IndexQuery& index,
                                          index::SymbolHash hash,
                                          Fid anchor);

/// Go-to-definition from a cursor: the definition sites (at its own
/// declaration, a virtual function nothing defines is defined by its
/// overrides), or —
/// standing on the definition itself, or when nothing defines the
/// symbol — the declarations (and sibling definitions) and, at a
/// specialization, the template it specializes, so definition and
/// declaration sites alternate; a namespace's blocks never do. The
/// cursor's own site drops out unless it is the only site the symbol
/// has.
std::vector<index::Site> definition(const index::IndexQuery& index,
                                    const index::IndexQuery::Cursor& cursor);

/// The mirror half of definition's alternation: declarations plus the
/// definition, minus the site the cursor stands on. At a function
/// defined with no declaration of its own, the declarations an edit to
/// one side may have parted from it (mismatched_declarations).
std::vector<index::Site> declaration(const index::IndexQuery& index,
                                     const index::IndexQuery::Cursor& cursor);

/// The references of the symbol under the cursor, weak ones included (a
/// template's call through an overload set or a dependent name, which
/// names its candidates only heuristically), optionally folding in its
/// declarations and definitions, deduplicated across the kinds — relations
/// of different kinds can share one anchor. A method's include the
/// references of the methods it overrides, a call through which may
/// dispatch to it, but not one naming that method by qualification
/// (`Base::f()`); a class's the names of its constructors.
std::vector<index::Site> references(const index::IndexQuery& index,
                                    const index::IndexQuery::Cursor& cursor,
                                    bool include_declaration);

struct PlacedHighlight {
    index::Site site;
    HighlightKind kind;
};

/// The highlights of the symbols under the cursor in the cursor's own
/// file, from the index serving it — a session's preamble region
/// included.
std::vector<PlacedHighlight> document_highlights(const index::IndexQuery& index,
                                                 const index::IndexQuery::Cursor& cursor);

std::vector<protocol::DocumentHighlight> to_protocol(llvm::ArrayRef<PlacedHighlight> highlights,
                                                     PositionEncoding encoding);

/// Go-to-type-definition of a cursor's symbol: the declarations its
/// type names — at a name of a member of a template's specialization,
/// the type the specialization gives it (TypeDefinition relations
/// spanning the cursor in the index serving its file); at a type, the
/// type itself; elsewhere the type it is declared with (the relations
/// without a range). An alias leads on to the type it names. The
/// cursor's own site drops out unless it is the only one.
std::vector<index::Site> type_definition(const index::IndexQuery& index,
                                         const index::IndexQuery::Cursor& cursor,
                                         index::SymbolHash symbol);

/// Whether the name at the cursor may mean `symbol` without naming it
/// for certain — a dependent name, an overload set's candidate, a
/// using-declaration's target: a WeakReference relation at the
/// cursor's name in the index serving its file.
bool names_weakly(const index::IndexQuery& index,
                  const index::IndexQuery::Cursor& cursor,
                  index::SymbolHash symbol);

/// What implements the symbol, each at its canonical site: the types
/// deriving from a class-like symbol, the overrides of a function, at
/// any depth; for a function nothing overrides, its definitions.
std::vector<index::IndexQuery::Located> implementation(const index::IndexQuery& index,
                                                       index::SymbolHash hash,
                                                       Fid anchor);

/// The same answers asked of every project knowing each symbol under the
/// cursor (index::PeerQueries).

/// Go-to-definition: each symbol answers for itself. A definition another
/// project compiles (a library's source beside the application including
/// its header) outranks the declarations this project alone can offer;
/// standing on a definition, or with none anywhere, the declarations
/// answer too (definition's alternation).
std::vector<index::Site> definition(const index::PeerQueries& queries,
                                    const index::IndexQuery::Cursor& cursor);

/// At most `limit` references (0 for no limit), those in the cursor's file
/// first when they are cut.
std::vector<index::Site> references(const index::PeerQueries& queries,
                                    const index::IndexQuery::Cursor& cursor,
                                    bool include_declaration,
                                    std::uint32_t limit);

std::vector<index::Site> declaration(const index::PeerQueries& queries,
                                     const index::IndexQuery::Cursor& cursor);

std::vector<index::Site> type_definition(const index::PeerQueries& queries,
                                         const index::IndexQuery::Cursor& cursor);

/// What implements the symbols, and what a name may mean
/// without naming it for certain (names_weakly) — a dependent name's
/// resolution, an overload set's candidates, a using-declaration's
/// targets — as an override implements a virtual function. The cursor's
/// own site drops out: standing on a function nothing overrides, there is
/// nowhere to go.
std::vector<index::Site> implementation(const index::PeerQueries& queries,
                                        const index::IndexQuery::Cursor& cursor);

}  // namespace clice::feature
