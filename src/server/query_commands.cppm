module;

#include "modules/prelude.h"

module clice:server.query_commands;

import :index.query;
import :index.rename;
import :server.editor_context;

namespace clice::query {

/// A command's answer, or the message for the user when the question
/// cannot be answered (an unknown file, an ambiguous name, an invalid
/// argument).
template <typename T>
using Outcome = std::expected<T, std::string>;

struct CompileCommandResult {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;

    /// Where the command came from: "database" (the file's own entry),
    /// "host" (a header compiled under an including source), "rule" (a
    /// rule's default command), "provisional" (borrowed from a nearby unit
    /// for a saved file the build counts as a provisional member),
    /// "inferred" (borrowed for a file outside the build) or
    /// "fallback" (nothing declared it).
    std::string source;

    /// Why querying the compiler failed, leaving `arguments` at driver
    /// level instead of the resolved frontend command; absent on success.
    std::optional<std::string> toolchain_error;
};

struct FileInfo {
    std::string path;
    std::string kind;
    std::optional<std::string> module_name;
};

struct ProjectFilesResult {
    std::vector<FileInfo> files;
    int total = 0;
};

struct DepEntry {
    std::string path;
    int depth = 0;
};

struct FileDepsResult {
    std::string file;
    std::vector<DepEntry> includes;
    std::vector<DepEntry> includers;
};

struct ImpactAnalysisResult {
    std::vector<std::string> direct_dependents;
    std::vector<std::string> transitive_dependents;
    std::vector<std::string> affected_modules;
};

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

/// What the commands read: the workspace's persisted index through an
/// index::IndexQuery and, for the build questions (compile commands, the file
/// set, include dependencies), the loaded build. Paths in and out are
/// absolute filesystem paths, not URIs.
struct Context {
    Project& project;
    EditorContext& contexts;
    const index::IndexQuery& query;

    /// Files a command was asked about that the index holds no shard for;
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
using Via =
    kota::meta::annotation<std::string, kota::meta::behavior::skip_if<kota::meta::pred::empty>>;

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
    index::BatchPlan plan;

    /// Whether anything keeps the edits from being written.
    bool blocked() const {
        return plan.blocked() ||
               llvm::any_of(result.renames, [](const RenameEntry& entry) { return entry.error; });
    }
};

/// The file's compile command as the editor would use it: pins and header
/// context included. Needs the build.
Outcome<CompileCommandResult> compile_command(Context& ctx, const Spelling& path);

/// The build's files, `filter` one of all, source, header, module. Needs
/// the build.
Outcome<ProjectFilesResult> project_files(Context& ctx, llvm::StringRef filter);

/// The files reachable from `path` along include edges, `direction` one of
/// includes, includers, both; `depth` levels at most, 0 meaning unbounded.
/// Needs the build.
Outcome<FileDepsResult>
    file_deps(Context& ctx, const Spelling& path, llvm::StringRef direction, int depth);

/// What a change to `path` reaches: its direct includers, the sources
/// hosting it, and the modules among them. Needs the build.
Outcome<ImpactAnalysisResult> impact_analysis(Context& ctx, const Spelling& path);

/// The symbols a locator (index::SymbolQuery) matches, best first, at most
/// `limit` of them.
Outcome<SymbolSearchResult> symbol_search(Context& ctx, llvm::StringRef text, std::size_t limit);

/// A `locator` (index::SymbolQuery) names one symbol: by the `symbol` an
/// answer carries, by a name query, or by a place. Several candidates are
/// an error listing them in Context::candidates; none is "symbol not
/// found".

/// The symbol's definition as text.
Outcome<ReadSymbolResult> read_symbol(Context& ctx, index::SymbolQuery locator);

/// The document-level symbols defined in the file.
Outcome<DocumentSymbolsResult> document_symbols(Context& ctx, const Spelling& path);

Outcome<DefinitionResult> definition(Context& ctx, index::SymbolQuery locator);

Outcome<ReferencesResult> references(Context& ctx,
                                     index::SymbolQuery locator,
                                     bool include_declaration);

/// `direction` one of callers, callees, both.
Outcome<CallGraphResult> call_graph(Context& ctx,
                                    index::SymbolQuery locator,
                                    llvm::StringRef direction);

/// Whether some unit of the build the index is to hold has no record in
/// it yet (see index::RenameScope::units_pending).
bool units_pending(Project& project);

/// Plan the renames together against the workspace on disk (see
/// index::plan_renames), each workspace file read once: what would change,
/// what the index cannot guarantee, and what keeps the change from being
/// safe. Nothing is written. A rename names its symbol by its whole name,
/// never a pattern; the overloads of one scope that name spells are
/// renamed together.
PlannedRename rename(Context& ctx, llvm::ArrayRef<RenameRequest> requests);

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

/// What implements the symbol (index::implementation): the
/// classes deriving from a class, the overrides of a function, at any
/// depth; a function nothing overrides, its definitions.
Outcome<ImplementationResult> implementation(Context& ctx, index::SymbolQuery locator);

/// `direction` one of supertypes, subtypes, both.
Outcome<TypeHierarchyResult> type_hierarchy(Context& ctx,
                                            index::SymbolQuery locator,
                                            llvm::StringRef direction);

}  // namespace clice::query
