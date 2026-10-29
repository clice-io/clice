module clice:feature.document_symbols;

import :compile.compilation;
import :feature.index_entries;
import :index.query;
import :semantic.symbol;
import :support.prelude;
import :syntax.lexical_scan;
import :syntax.position;
import :syntax.source_text;

namespace clice::feature {

/// Corresponds to the `[document_symbols]` section in clice.toml.
struct DocumentSymbolsOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "List the `#define`s the file writes; an include guard's never "
                         "lists.")
    <bool> macros = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "List what a function's body declares: its variables, types and "
                         "lambdas, under the function.")
    <bool> locals = true;
};

struct DocumentSymbol {
    std::string name;
    std::string detail;
    SymbolKind kind = SymbolKind::Invalid;
    LocalSourceRange range;
    LocalSourceRange selection_range;
    bool deprecated = false;
    std::vector<DocumentSymbol> children;

    /// The entity it declares (EntityTable), the index's symbol hash; 0 for
    /// a `#define`, a macro invocation, a mark and a module declaration.
    std::uint64_t entity = 0;
};

/// One declaration, macro or module declaration the outline lists, before
/// it is named from the text and nested: the AST and the index each give
/// these, and outline() makes both into the same tree.
struct OutlineEntry {
    /// The name the declaration goes by (display::symbol_name); outline()
    /// adds the qualifier and template arguments the text writes.
    std::string name;
    std::string detail;
    SymbolKind kind = SymbolKind::Invalid;
    LocalSourceRange range;
    LocalSourceRange selection_range;
    bool deprecated = false;

    /// The entity declared and the one it is named in (EntityTable), 0
    /// for macros and modules.
    std::uint64_t entity = 0;
    std::uint64_t parent = 0;

    bool definition = false;

    /// The name is a presentation, `(anonymous struct)`.
    bool unnamed = false;

    /// The name comes out of a macro's body or a token paste and is
    /// written nowhere: the selection range is the macro invocation's.
    bool from_macro = false;
};

/// Whether the outline lists a declaration of `kind`: a nameless one only
/// when it is a scope, a template parameter never.
bool outline_lists(SymbolKind kind, bool unnamed, bool template_parameter);

/// The outline the entries make in the file `source` holds, with its
/// `#define` directives (`macros`, those the preprocessor read), module
/// declarations and `#pragma mark`s (`marks`): the entries are named as the
/// text writes them, members of one macro invocation listed under it,
/// nested by where they are written, and sections made of the marks.
auto outline(std::vector<OutlineEntry> entries,
             SourceText& source,
             llvm::ArrayRef<LexicalInfo::Directive> macros,
             llvm::ArrayRef<LexicalInfo::ModuleDeclaration> modules,
             llvm::ArrayRef<LexicalInfo::Directive> marks,
             const DocumentSymbolsOptions& options) -> std::vector<DocumentSymbol>;

/// The outline of the focus file, a `#define` in it only outside the
/// inactive regions. The overload without a regions argument scans the
/// unit itself — correct only for units compiled without a PCH, as with
/// semantic_tokens.
auto document_symbols(CompilationUnit& unit, const DocumentSymbolsOptions& options)
    -> std::vector<DocumentSymbol>;
auto document_symbols(CompilationUnit& unit,
                      llvm::ArrayRef<std::uint32_t> inactive_regions,
                      const DocumentSymbolsOptions& options) -> std::vector<DocumentSymbol>;

/// The outline's SymbolKind -> LSP mapping, exhaustive over the enum so a
/// new kind fails to compile rather than fall into a default.
auto to_protocol(SymbolKind kind) -> protocol::SymbolKind;

/// How a client takes document symbols: as a tree when it declares
/// hierarchicalDocumentSymbolSupport, flat (`SymbolInformation`)
/// otherwise; in every kind when it declares a set of them (and so takes
/// others gracefully), in the protocol's first 18 otherwise.
struct SymbolClient {
    bool hierarchical = false;
    bool all_kinds = false;
};

auto to_protocol(llvm::ArrayRef<DocumentSymbol> symbols,
                 const PositionMap& map,
                 PositionEncoding encoding,
                 const SymbolClient& client) -> std::vector<protocol::DocumentSymbol>;

/// The symbols flat, for a client taking no tree: each in `uri`, naming the
/// symbol it nests in.
auto to_symbol_information(llvm::ArrayRef<DocumentSymbol> symbols,
                           const PositionMap& map,
                           PositionEncoding encoding,
                           const SymbolClient& client,
                           llvm::StringRef uri) -> std::vector<protocol::SymbolInformation>;

/// The outline of the file `source` holds, from its index entries: the
/// AST's (outline()) without details, a macro's parameters aside.
auto document_symbols(SourceText& source,
                      const IndexEntries& entries,
                      IndexSymbolResolver resolve,
                      const DocumentSymbolsOptions& options) -> std::vector<DocumentSymbol>;

/// The same from the index serving the file (see semantic_tokens).
auto document_symbols(SourceText& source,
                      const index::IndexQuery& query,
                      const index::IndexSource& index,
                      const DocumentSymbolsOptions& options) -> std::vector<DocumentSymbol>;

}  // namespace clice::feature
