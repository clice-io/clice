#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "sched/context.h"
#include "server/service/query.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice::query {

/// What the commands read: the workspace's persisted index through an
/// IndexQuery and, for the build questions (compile commands, the file
/// set, include dependencies), the loaded build. Paths in and out are
/// absolute filesystem paths, not URIs.
struct Context {
    Workspace& workspace;
    ContextResolver& contexts;
    const IndexQuery& query;

    /// Files a command was asked about that the index holds no rows for;
    /// the caller reports them next to the rows the query withheld.
    std::vector<std::string> unindexed;
};

/// A command's answer, or the message for the user when the question
/// cannot be answered (an unknown file, an ambiguous name, an invalid
/// argument).
template <typename T>
using Outcome = std::expected<T, std::string>;

/// How a symbol is named: by its id (the `#<hex>` the answers carry); by
/// name, case-insensitively, optionally narrowed to a path; or by path and
/// 1-based line.
struct SymbolLocatorParams {
    std::optional<std::string> name;
    std::optional<std::string> path;
    std::optional<int> line;
    std::optional<std::string> symbol;
};

struct CompileCommandResult {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;

    /// Where the command came from: "database" (the file's own entry),
    /// "host" (a header compiled under an including source), "rule" (a
    /// rule's default command), "inferred" (borrowed from a nearby unit)
    /// or "fallback" (nothing declared it).
    std::string source;
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
    std::string kind;
    std::string file;
    int line = 0;
    std::optional<std::string> container;
    std::string symbol_id;
};

struct SymbolSearchResult {
    std::vector<SymbolEntry> symbols;
};

struct ReadSymbolResult {
    std::string name;
    std::string kind;
    std::string file;
    int start_line = 0;
    int end_line = 0;
    std::string text;
    std::string symbol_id;
};

struct DocumentSymbolEntry {
    std::string name;
    std::string kind;
    int start_line = 0;
    int end_line = 0;
    std::string symbol_id;
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
    std::string symbol_id;
    std::optional<LocationEntry> definition;
};

struct ReferenceEntry {
    std::string file;
    int line = 0;
    std::string context;
};

struct ReferencesResult {
    std::string name;
    std::string kind;
    std::string symbol_id;
    std::vector<ReferenceEntry> references;
    int total = 0;
};

struct GraphEntry {
    std::string name;
    std::string kind;
    std::string file;
    int line = 0;
    std::string symbol_id;
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

/// The file's compile command as the editor would use it: pins and header
/// context included. Needs the build.
Outcome<CompileCommandResult> compile_command(Context& ctx, llvm::StringRef path);

/// The build's files, `filter` one of all, source, header, module. Needs
/// the build.
Outcome<ProjectFilesResult> project_files(Context& ctx, llvm::StringRef filter);

/// The files reachable from `path` along include edges, `direction` one of
/// includes, includers, both; `depth` levels at most, 0 meaning unbounded.
/// Needs the build.
Outcome<FileDepsResult>
    file_deps(Context& ctx, llvm::StringRef path, llvm::StringRef direction, int depth);

/// What a change to `path` reaches: its direct includers, the sources
/// hosting it, and the modules among them. Needs the build.
Outcome<ImpactAnalysisResult> impact_analysis(Context& ctx, llvm::StringRef path);

/// Symbols whose name contains `text`, best matches first, at most `limit`
/// of them, narrowed to `kinds` (SymbolKind names) when non-empty.
Outcome<SymbolSearchResult> symbol_search(Context& ctx,
                                          llvm::StringRef text,
                                          std::size_t limit,
                                          llvm::ArrayRef<std::string> kinds);

/// The symbol's definition as text.
Outcome<ReadSymbolResult> read_symbol(Context& ctx, const SymbolLocatorParams& locator);

/// The document-level symbols defined in the file.
Outcome<DocumentSymbolsResult> document_symbols(Context& ctx, llvm::StringRef path);

Outcome<DefinitionResult> definition(Context& ctx, const SymbolLocatorParams& locator);

Outcome<ReferencesResult> references(Context& ctx,
                                     const SymbolLocatorParams& locator,
                                     bool include_declaration);

/// `direction` one of callers, callees, both.
Outcome<CallGraphResult> call_graph(Context& ctx,
                                    const SymbolLocatorParams& locator,
                                    llvm::StringRef direction);

/// `direction` one of supertypes, subtypes, both.
Outcome<TypeHierarchyResult> type_hierarchy(Context& ctx,
                                            const SymbolLocatorParams& locator,
                                            llvm::StringRef direction);

}  // namespace clice::query
