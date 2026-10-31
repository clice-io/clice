module;

/// The command line's answers about symbols and files, from the index
/// alone: what `clice query` prints, the renames `clice refactor` plans, the
/// symbols `clice analyze` and `clice index` look up.

module clice:feature.query;

import :feature.documentation;
import :feature.rename;
import :index.query;
import :index.symbol;
import :index.symbol_query;
import :support.prelude;
import :vfs.ids;
import :vfs.path;

namespace clice::feature::query {

/// A command's answer, or the message for the user when the question
/// cannot be answered (an unknown file, an ambiguous name, an invalid
/// argument).
template <typename T>
using Outcome = std::expected<T, std::string>;

struct SymbolEntry {
    std::string name;
    /// A function's parameters and qualifiers, which tell its overloads
    /// apart; null for any other symbol.
    std::optional<std::string> signature;
    std::string kind;
    std::string file;
    int line = 0;
    std::optional<std::string> container;

    /// The locator naming the symbol back: its `#<hex>` id, with the
    /// file holding its entries for a symbol only some files see.
    std::string symbol;
};

struct SymbolSearchResult {
    std::vector<SymbolEntry> symbols;
};

/// What the answers read: the project's index through an
/// index::IndexQuery, its root, the directory relative paths name files
/// under, and how documentation comments are read. Paths out are absolute
/// filesystem paths, not URIs.
struct Context {
    const index::IndexQuery& query;

    CanonicalRef root;

    /// The current directory of a command-line run, which the relative paths
    /// it is given are relative to; empty for a server, whose requests name
    /// files absolute.
    Spelling cwd;

    const DocumentationOptions& documentation;

    /// Files an answer was asked about that the index holds no shard for;
    /// the caller reports them next to the shards the query withheld.
    std::vector<std::string> unindexed;

    /// The symbols a locator named when it had to name one; the caller
    /// lists them next to the error.
    std::vector<SymbolEntry> candidates;
};

struct ReadSymbolResult {
    std::string name;
    std::string kind;
    std::string file;
    int start_line = 0;
    int end_line = 0;
    /// What its documentation comment says, as markdown; null without one.
    std::optional<std::string> documentation;
    std::string text;
    std::string symbol;
};

struct DocumentSymbolEntry {
    std::string name;
    std::string kind;
    int start_line = 0;
    int end_line = 0;
    std::string symbol;
};

struct DocumentSymbolsResult {
    std::vector<DocumentSymbolEntry> symbols;
};

struct LocationEntry {
    std::string file;
    int start_line = 0;
    int end_line = 0;
    std::string text;
};

struct DefinitionResult {
    std::string name;
    std::string kind;
    std::string symbol;
    /// What its documentation comment says, as markdown; null without one.
    std::optional<std::string> documentation;
    /// Its definition, or its declaration when nothing defines it.
    std::optional<LocationEntry> definition;
};

/// "instantiation" or "expansion" for what only a template instantiation or
/// a macro expansion resolves, which the written code leaves open; absent
/// for the written code.
using Via = meta::annotation<std::string, meta::behavior::skip_if<meta::pred::empty>>;

struct ReferenceEntry {
    std::string file;
    int line = 0;
    std::string context;
    /// The qualified name of the function it lies in; null outside any.
    std::optional<std::string> function;
    Via via;
};

struct ReferencesResult {
    std::string name;
    std::string kind;
    std::string symbol;
    std::vector<ReferenceEntry> references;
    int total = 0;
};

struct GraphEntry {
    std::string name;
    /// A function's parameters and qualifiers, which tell its overloads
    /// apart; null for any other symbol.
    std::optional<std::string> signature;
    std::string kind;
    std::string file;
    int line = 0;
    std::string symbol;
    /// For a caller or a callee, set when only instantiations make the
    /// call.
    Via via;
};

struct CallGraphResult {
    GraphEntry root;
    std::vector<GraphEntry> callers;
    std::vector<GraphEntry> callees;
};

struct TypeHierarchyResult {
    GraphEntry root;
    std::vector<GraphEntry> supertypes;
    std::vector<GraphEntry> subtypes;
};

struct ImplementationResult {
    GraphEntry root;
    std::vector<GraphEntry> implementations;
};

/// One rename asked for: the symbol, by a locator, and its new name.
struct RenameRequest {
    /// The locator as written, for the answer.
    std::string text;
    index::SymbolQuery locator;
    std::string new_name;

    /// The line of the rename list asking for it; absent on the command
    /// line.
    std::optional<int> line;
};

/// One rename of the answer: the symbol its locator found, and what it
/// alone brings — the rest is the batch's.
struct RenameEntry {
    std::optional<int> line;
    std::string locator;
    std::string name;
    std::string kind;
    std::string symbol;
    std::string new_name;

    /// The tokens it replaces.
    int edits = 0;

    /// What keeps it from being made, which stops the batch.
    std::vector<std::string> conflicts;

    std::vector<std::string> warnings;

    /// Why the locator names no symbol to rename, which stops the batch.
    std::optional<std::string> error;

    /// The symbols a locator naming several left to choose from.
    std::optional<std::vector<SymbolEntry>> candidates;
};

/// One token a rename replaces: 1-based line and byte column.
struct RenameEditEntry {
    int line = 0;
    int column = 0;
    std::string name;
    std::string new_name;

    /// Reached through a name the index resolved heuristically.
    bool heuristic = false;
};

struct RenameFileEntry {
    std::string file;
    std::vector<RenameEditEntry> edits;
};

/// A token spelling an old name that the renames leave alone.
struct RenameNoteEntry {
    std::string file;
    int line = 0;
    int column = 0;
    std::string name;
    std::string reason;
    std::string text;
};

struct RenameResult {
    /// Whether the edits were written.
    bool applied = false;

    std::vector<RenameEntry> renames;
    std::vector<RenameFileEntry> files;
    std::vector<RenameNoteEntry> unconfirmed;

    /// What keeps the renames from being written beyond each one's own: a
    /// token two of them would change to different names, a file the
    /// writing could not rewrite.
    std::vector<std::string> conflicts;

    /// Files spelling an old or a new name whose index is not current; the
    /// renames are not written while any stands.
    std::vector<std::string> stale;
};

/// Renames computed but not applied: the answer, and the plan their edits
/// come from.
struct PlannedRename {
    RenameResult result;
    BatchPlan plan;

    /// Whether anything keeps the edits from being written.
    bool blocked() const {
        return plan.blocked() || ranges::any_of(result.renames, [](const RenameEntry& entry) {
                   return entry.error.has_value();
               });
    }
};

/// The files a rename batch reads and edits, as the caller knows them: those
/// its scan reads (RenameScope::files), the ones it may edit
/// (RenameScope::editable), and whether the build has units the index holds
/// no record of yet (RenameScope::units_pending).
struct RenameFiles {
    std::vector<std::string> files;

    llvm::function_ref<bool(llvm::StringRef path)> editable;

    bool units_pending = false;
};

/// The symbols a locator (index::SymbolQuery) matches, best first, at most
/// `limit` of them.
Outcome<SymbolSearchResult> symbol_search(Context& ctx, llvm::StringRef text, std::size_t limit);

/// A `locator` (index::SymbolQuery) names one symbol: by the `symbol` an
/// answer carries, by a name query, or by a place. Several candidates are
/// an error listing them in Context::candidates; none is "symbol not
/// found".

/// The symbol's definition as text.
Outcome<ReadSymbolResult> read_symbol(Context& ctx, index::SymbolQuery locator);

/// The document-level symbols defined in the file, its text lexed in the
/// dialect `dialect` gives.
Outcome<DocumentSymbolsResult>
    document_symbols(Context& ctx,
                     const Spelling& path,
                     llvm::function_ref<const clang::LangOptions&(Fid file)> dialect);

Outcome<DefinitionResult> definition(Context& ctx, index::SymbolQuery locator);

Outcome<ReferencesResult> references(Context& ctx,
                                     index::SymbolQuery locator,
                                     bool include_declaration);

/// `direction` one of callers, callees, both.
Outcome<CallGraphResult> call_graph(Context& ctx,
                                    index::SymbolQuery locator,
                                    llvm::StringRef direction);

/// Plan the renames together against `files` on disk (see
/// plan_renames), each of its files read once: what would change, what the
/// index cannot guarantee, and what keeps the change from being safe.
/// Nothing is written. A rename names its symbol by its whole name, never
/// a pattern; the overloads of one scope that name spells are renamed
/// together.
PlannedRename rename(Context& ctx, llvm::ArrayRef<RenameRequest> requests, RenameFiles files);

/// The symbols one declaration a locator names stands for: the symbol,
/// or, for a name the overloads of one scope spell, all of them; macros
/// of that name are left out. Named by its whole name, never a pattern;
/// several other candidates are an error listing them in
/// Context::candidates.
Outcome<std::vector<index::SymbolHash>> declaration(Context& ctx, index::SymbolQuery locator);

/// Every symbol a locator names, a bare name taken as the whole name, with
/// where it was found; none is "symbol not found".
Outcome<std::vector<index::IndexQuery::Located>> named_symbols(Context& ctx,
                                                               index::SymbolQuery locator);

/// What implements the symbol (feature::implementation): the
/// classes deriving from a class, the overrides of a function, at any
/// depth; a function nothing overrides, its definitions.
Outcome<ImplementationResult> implementation(Context& ctx, index::SymbolQuery locator);

/// `direction` one of supertypes, subtypes, both.
Outcome<TypeHierarchyResult> type_hierarchy(Context& ctx,
                                            index::SymbolQuery locator,
                                            llvm::StringRef direction);

}  // namespace clice::feature::query
