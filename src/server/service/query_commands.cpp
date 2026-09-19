#include "server/service/query_commands.h"

#include <format>

#include "index/serialization.h"
#include "support/filesystem.h"

#include "kota/meta/enum.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice::query {

namespace {

std::string kind_name(SymbolKind kind) {
    return std::string(kota::meta::enum_name(static_cast<SymbolKind::Kind>(kind), "Unknown"));
}

std::string symbol_id(index::SymbolHash hash) {
    return std::format("#{:016x}", hash);
}

/// The hash a `#<hex>` id names; nullopt for anything else.
std::optional<index::SymbolHash> parse_symbol_id(llvm::StringRef id) {
    index::SymbolHash hash = 0;
    if(!id.consume_front("#") || id.getAsInteger(16, hash) || index::reserved_key(hash)) {
        return std::nullopt;
    }
    return hash;
}

bool is_header(llvm::StringRef path) {
    auto ext = llvm::sys::path::extension(path);
    return ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh";
}

/// The 1-based lines a site spans, as the answers spell positions.
struct Lines {
    int start;
    int end;
};

Lines lines_of(const Site& site) {
    return {.start = static_cast<int>(site.begin.line) + 1,
            .end = static_cast<int>(site.end.line) + 1};
}

/// The file a path names in the index. An error for a path that is not a
/// file; nullopt (noted as unindexed) for one the index has no rows for.
Outcome<std::optional<Fid>> indexed_file(Context& ctx, llvm::StringRef path) {
    if(!llvm::sys::fs::is_regular_file(path)) {
        return std::unexpected(std::format("no such file: {}", std::string_view(path)));
    }
    auto file = ctx.workspace.file_table.find(path);
    if(!file || !ctx.workspace.project_index.shards.contains(*file)) {
        ctx.unindexed.emplace_back(path);
        return std::nullopt;
    }
    return file;
}

/// Anchor a query's place in the workspace: its path becomes absolute the
/// way the command's own path arguments do, and must be an indexed file.
/// Nullopt when the file is not indexed, which the caller answers as not
/// found with the path noted.
Outcome<bool> anchor_place(Context& ctx, index::SymbolQuery& query) {
    if(!query.position) {
        return true;
    }
    auto& place = *query.position;
    llvm::SmallString<256> absolute(
        path::is_absolute(place.path)
            ? place.path
            : path::join(ctx.workspace.config.workspace_root, place.path));
    path::remove_dots(absolute, /*remove_dot_dot=*/true);
    place.path = absolute.str();
    path::canonicalize(place.path);
    auto file = indexed_file(ctx, place.path);
    if(!file) {
        return std::unexpected(file.error());
    }
    return file->has_value();
}

/// Resolve a locator to exactly one symbol: no candidate is an unknown
/// symbol, several ask the caller to disambiguate by id. A locator naming
/// a path the index has no rows for answers as unknown and notes the path.
Outcome<IndexQuery::Located> resolve_unique(Context& ctx, const SymbolLocatorParams& params) {
    index::SymbolQuery query;
    if(params.symbol) {
        auto parsed = index::SymbolQuery::parse(*params.symbol);
        if(!parsed || !parsed->handle) {
            return std::unexpected(std::format("invalid symbol id: {}", *params.symbol));
        }
        query = std::move(*parsed);
    } else if(params.name) {
        auto parsed = index::SymbolQuery::parse(*params.name);
        if(!parsed) {
            return std::unexpected(parsed.error());
        }
        query = std::move(*parsed);
    }
    if(params.line && *params.line <= 0) {
        return std::unexpected("line must be positive");
    }
    if(params.path) {
        auto file = indexed_file(ctx, *params.path);
        if(!file) {
            return std::unexpected(file.error());
        }
        if(!*file) {
            return std::unexpected("symbol not found");
        }
        if(params.name) {
            query.paths.push_back(*params.path);
        } else if(params.line && !params.symbol) {
            query.position = {.path = *params.path, .line = *params.line};
        }
    }
    if(!params.symbol && !params.name && !query.position) {
        return std::unexpected("name a symbol with --name, --symbol, or --path and --line");
    }
    auto anchored = anchor_place(ctx, query);
    if(!anchored) {
        return std::unexpected(anchored.error());
    }
    if(!*anchored) {
        return std::unexpected("symbol not found");
    }
    auto candidates = ctx.query.locate(query);
    if(candidates.empty()) {
        return std::unexpected("symbol not found");
    }
    if(candidates.size() > 1) {
        std::string listed;
        for(auto& candidate: llvm::ArrayRef(candidates).take_front(5)) {
            listed += std::format("{}{} ({})",
                                  listed.empty() ? "" : ", ",
                                  ctx.query.qualified_name(candidate.symbol.hash),
                                  symbol_id(candidate.symbol.hash));
        }
        return std::unexpected(std::format("ambiguous: {} candidates, use --symbol to pick one: {}",
                                           candidates.size(),
                                           listed));
    }
    return std::move(candidates[0]);
}

/// The files reachable from `root` along `adjacent`, breadth first and
/// each once: direct neighbours at depth 1, `max_depth` levels at most,
/// 0 meaning unbounded.
std::vector<DepEntry> collect_deps(Workspace& ws,
                                   Fid root,
                                   int max_depth,
                                   llvm::function_ref<llvm::SmallVector<Fid>(Fid)> adjacent) {
    std::vector<DepEntry> entries;
    llvm::SmallVector<std::pair<Fid, int>> queue{
        {root, 0}
    };
    llvm::DenseSet<Fid> visited{root};
    for(std::size_t i = 0; i < queue.size(); i += 1) {
        auto [id, depth] = queue[i];
        if(max_depth > 0 && depth >= max_depth) {
            continue;
        }
        for(auto next: adjacent(id)) {
            if(!visited.insert(next).second) {
                continue;
            }
            queue.push_back({next, depth + 1});
            entries.push_back({.path = ws.file_table.resolve(next).str(), .depth = depth + 1});
        }
    }
    return entries;
}

template <typename Entry>
Entry graph_entry(const IndexQuery::Located& located) {
    return {
        .name = located.symbol.display_name(),
        .kind = kind_name(located.symbol.kind),
        .file = std::string(located.site.path),
        .line = lines_of(located.site).start,
        .symbol_id = symbol_id(located.symbol.hash),
    };
}

/// The relation targets of `root` of `kind` as graph entries, each
/// resolved to its canonical site.
void collect_targets(Context& ctx,
                     index::SymbolHash root,
                     RelationKind kind,
                     std::vector<GraphEntry>& into) {
    for(auto target: ctx.query.targets(root, kind)) {
        if(auto located = ctx.query.resolve(target)) {
            into.push_back(graph_entry<GraphEntry>(*located));
        }
    }
}

}  // namespace

Outcome<CompileCommandResult> compile_command(Context& ctx, llvm::StringRef path) {
    if(!llvm::sys::fs::is_regular_file(path)) {
        return std::unexpected(std::format("no such file: {}", std::string_view(path)));
    }
    // The editor compiles such a header under a synthesized preamble, a
    // cache artifact a read-only reader cannot produce; the host's bare
    // command would be a different compile.
    auto needs_context = [&](Fid file) {
        auto* choice = ctx.contexts.selection(ContextUse::Editor, file);
        return ctx.contexts.header_mode(path, file) == HeaderMode::NeedsContext ||
               (choice && choice->host_path_id.valid() && choice->occurrence.has_value());
    };
    if(auto file = ctx.workspace.file_table.find(path); file && needs_context(*file)) {
        return std::unexpected(std::format(
            "{} compiles only under a synthesized header context, which needs an editor session",
            std::string_view(path)));
    }
    CompileCommandResult result{.file = std::string(path)};
    auto source =
        ctx.contexts.resolve_command(path, result.directory, result.arguments, ContextUse::Editor);
    switch(source) {
        case CommandSource::CDBExact: result.source = "database"; break;
        case CommandSource::IncludeGraph: result.source = "host"; break;
        case CommandSource::Default: result.source = "rule"; break;
        case CommandSource::Inferred: result.source = "inferred"; break;
        case CommandSource::Fallback: result.source = "fallback"; break;
    }
    return result;
}

Outcome<ProjectFilesResult> project_files(Context& ctx, llvm::StringRef filter) {
    if(!llvm::is_contained<llvm::StringRef>({"all", "source", "header", "module"}, filter)) {
        return std::unexpected(
            std::format("invalid filter '{}': expected all, source, header or module",
                        std::string_view(filter)));
    }
    auto& ws = ctx.workspace;
    ProjectFilesResult result;
    llvm::DenseSet<Fid> seen;
    for(auto member: ws.build.members()) {
        auto file_path = ws.file_table.resolve(member);
        if(file_path.empty() || !seen.insert(member).second) {
            continue;
        }
        auto module_name = ws.dep_graph.module_of(member);
        llvm::StringRef kind = !module_name.empty()   ? "module"
                               : is_header(file_path) ? "header"
                                                      : "source";
        if(filter != "all" && filter != kind) {
            continue;
        }
        FileInfo info{.path = file_path.str(), .kind = kind.str()};
        if(!module_name.empty()) {
            info.module_name = module_name.str();
        }
        result.files.push_back(std::move(info));
    }
    if(filter == "all" || filter == "header") {
        for(auto path_id: llvm::make_first_range(ws.project_index.shards)) {
            auto path = ws.file_table.resolve(path_id);
            if(!seen.contains(path_id) && is_header(path)) {
                seen.insert(path_id);
                result.files.push_back({.path = path.str(), .kind = "header"});
            }
        }
    }
    result.total = static_cast<int>(result.files.size());
    return result;
}

Outcome<FileDepsResult>
    file_deps(Context& ctx, llvm::StringRef path, llvm::StringRef direction, int depth) {
    if(!llvm::is_contained<llvm::StringRef>({"includes", "includers", "both"}, direction)) {
        return std::unexpected(
            std::format("invalid direction '{}': expected includes, includers or both",
                        std::string_view(direction)));
    }
    if(depth < 0) {
        return std::unexpected("depth must not be negative");
    }
    if(!llvm::sys::fs::is_regular_file(path)) {
        return std::unexpected(std::format("no such file: {}", std::string_view(path)));
    }
    auto& ws = ctx.workspace;
    FileDepsResult result{.file = std::string(path)};
    auto file = ws.file_table.find(path);
    if(!file) {
        return result;
    }
    if(direction != "includers") {
        result.includes = collect_deps(ws, *file, depth, [&](Fid id) {
            return ws.dep_graph.get_all_includes(id);
        });
    }
    if(direction != "includes") {
        result.includers = collect_deps(ws, *file, depth, [&](Fid id) {
            return llvm::SmallVector<Fid>(ws.dep_graph.get_includers(id));
        });
    }
    return result;
}

Outcome<ImpactAnalysisResult> impact_analysis(Context& ctx, llvm::StringRef path) {
    if(!llvm::sys::fs::is_regular_file(path)) {
        return std::unexpected(std::format("no such file: {}", std::string_view(path)));
    }
    auto& ws = ctx.workspace;
    ImpactAnalysisResult result;
    auto file = ws.file_table.find(path);
    if(!file) {
        return result;
    }
    auto direct = ws.dep_graph.get_includers(*file);
    llvm::DenseSet<Fid> seen{*file};
    for(auto includer: direct) {
        result.direct_dependents.push_back(ws.file_table.resolve(includer).str());
        seen.insert(includer);
    }
    auto hosts = ws.dep_graph.find_host_sources(*file);
    for(auto host: hosts) {
        if(seen.insert(host).second) {
            result.transitive_dependents.push_back(ws.file_table.resolve(host).str());
        }
    }
    for(auto host: hosts) {
        auto module_name = ws.dep_graph.module_of(host);
        if(!module_name.empty()) {
            result.affected_modules.push_back(module_name.str());
        }
    }
    auto module_name = ws.dep_graph.module_of(*file);
    if(!module_name.empty()) {
        result.affected_modules.push_back(module_name.str());
    }
    return result;
}

Outcome<SymbolSearchResult> symbol_search(Context& ctx,
                                          llvm::StringRef text,
                                          std::size_t limit,
                                          llvm::ArrayRef<std::string> kinds) {
    auto query = index::SymbolQuery::parse(text);
    if(!query) {
        return std::unexpected(query.error());
    }
    for(auto& kind: kinds) {
        auto parsed = index::SymbolQuery::parse_kind(kind);
        if(!parsed) {
            return std::unexpected(std::format("unknown symbol kind '{}'", kind));
        }
        query->kinds.push_back(*parsed);
    }
    SymbolSearchResult result;
    auto anchored = anchor_place(ctx, *query);
    if(!anchored) {
        return std::unexpected(anchored.error());
    }
    if(!*anchored) {
        return result;
    }
    auto located = query->by_pattern() ? ctx.query.search(*query, limit) : ctx.query.locate(*query);
    // A locator names its symbols outright; the filters still apply.
    if(!query->by_pattern()) {
        llvm::erase_if(located, [&](const IndexQuery::Located& hit) {
            if(!query->kinds.empty() && !llvm::is_contained(query->kinds, hit.symbol.kind)) {
                return true;
            }
            return !query->paths.empty() &&
                   llvm::none_of(query->paths, [&](const std::string& wanted) {
                       return index::path_matches(wanted, hit.site.path);
                   });
        });
        if(located.size() > limit) {
            located.resize(limit);
        }
    }
    for(auto& hit: located) {
        auto entry = graph_entry<SymbolEntry>(hit);
        if(auto container = ctx.query.container_name(hit.symbol.hash); !container.empty()) {
            entry.container = std::move(container);
        }
        result.symbols.push_back(std::move(entry));
    }
    return result;
}

Outcome<ReadSymbolResult> read_symbol(Context& ctx, const SymbolLocatorParams& locator) {
    auto resolved = resolve_unique(ctx, locator);
    if(!resolved) {
        return std::unexpected(resolved.error());
    }
    auto definition = ctx.query.definition_text(resolved->symbol.hash);
    if(!definition) {
        return std::unexpected("definition not found");
    }
    auto lines = lines_of(definition->extent);
    return ReadSymbolResult{
        .name = resolved->symbol.display_name(),
        .kind = kind_name(resolved->symbol.kind),
        .file = std::string(definition->extent.path),
        .start_line = lines.start,
        .end_line = lines.end,
        .text = std::move(definition->text),
        .symbol_id = symbol_id(resolved->symbol.hash),
    };
}

Outcome<DocumentSymbolsResult> document_symbols(Context& ctx, llvm::StringRef path) {
    auto is_document_level = [](SymbolKind kind) {
        return kind == SymbolKind::Namespace || kind == SymbolKind::Class ||
               kind == SymbolKind::Struct || kind == SymbolKind::Union ||
               kind == SymbolKind::Enum || kind == SymbolKind::Type || kind == SymbolKind::Field ||
               kind == SymbolKind::EnumMember || kind == SymbolKind::Function ||
               kind == SymbolKind::Method || kind == SymbolKind::Variable ||
               kind == SymbolKind::Macro || kind == SymbolKind::Concept ||
               kind == SymbolKind::Module || kind == SymbolKind::Operator ||
               kind == SymbolKind::Attribute;
    };
    auto file = indexed_file(ctx, path);
    if(!file) {
        return std::unexpected(file.error());
    }
    DocumentSymbolsResult result;
    if(!*file) {
        return result;
    }
    for(auto& located: ctx.query.definitions_in(**file)) {
        if(!is_document_level(located.symbol.kind)) {
            continue;
        }
        auto lines = lines_of(located.site);
        result.symbols.push_back({
            .name = located.symbol.display_name(),
            .kind = kind_name(located.symbol.kind),
            .start_line = lines.start,
            .end_line = lines.end,
            .symbol_id = symbol_id(located.symbol.hash),
        });
    }
    return result;
}

Outcome<DefinitionResult> definition(Context& ctx, const SymbolLocatorParams& locator) {
    auto resolved = resolve_unique(ctx, locator);
    if(!resolved) {
        return std::unexpected(resolved.error());
    }
    DefinitionResult result{
        .name = resolved->symbol.display_name(),
        .kind = kind_name(resolved->symbol.kind),
        .symbol_id = symbol_id(resolved->symbol.hash),
    };
    if(auto definition = ctx.query.definition_text(resolved->symbol.hash)) {
        auto lines = lines_of(definition->extent);
        result.definition = LocationEntry{
            .file = std::string(definition->extent.path),
            .start_line = lines.start,
            .end_line = lines.end,
            .text = std::move(definition->text),
        };
    }
    return result;
}

Outcome<ReferencesResult> references(Context& ctx,
                                     const SymbolLocatorParams& locator,
                                     bool include_declaration) {
    auto resolved = resolve_unique(ctx, locator);
    if(!resolved) {
        return std::unexpected(resolved.error());
    }
    ReferencesResult result{
        .name = resolved->symbol.display_name(),
        .kind = kind_name(resolved->symbol.kind),
        .symbol_id = symbol_id(resolved->symbol.hash),
    };
    IndexQuery::Cursor cursor{.symbol = resolved->symbol.hash, .site = resolved->site};
    for(auto& site: ctx.query.references(cursor, include_declaration)) {
        result.references.push_back({
            .file = std::string(site.path),
            .line = lines_of(site).start,
            .context = ctx.query.context_line(site),
        });
    }
    result.total = static_cast<int>(result.references.size());
    return result;
}

Outcome<CallGraphResult> call_graph(Context& ctx,
                                    const SymbolLocatorParams& locator,
                                    llvm::StringRef direction) {
    if(!llvm::is_contained<llvm::StringRef>({"callers", "callees", "both"}, direction)) {
        return std::unexpected(
            std::format("invalid direction '{}': expected callers, callees or both",
                        std::string_view(direction)));
    }
    auto resolved = resolve_unique(ctx, locator);
    if(!resolved) {
        return std::unexpected(resolved.error());
    }
    CallGraphResult result{.root = graph_entry<GraphEntry>(*resolved)};
    auto collect = [&](RelationKind kind, std::vector<GraphEntry>& into) {
        for(auto& group: ctx.query.grouped(resolved->symbol.hash, kind)) {
            if(auto located = ctx.query.resolve(group.symbol)) {
                into.push_back(graph_entry<GraphEntry>(*located));
            }
        }
    };
    if(direction != "callees") {
        collect(RelationKind::Caller, result.callers);
    }
    if(direction != "callers") {
        collect(RelationKind::Callee, result.callees);
    }
    return result;
}

Outcome<TypeHierarchyResult> type_hierarchy(Context& ctx,
                                            const SymbolLocatorParams& locator,
                                            llvm::StringRef direction) {
    if(!llvm::is_contained<llvm::StringRef>({"supertypes", "subtypes", "both"}, direction)) {
        return std::unexpected(
            std::format("invalid direction '{}': expected supertypes, subtypes or both",
                        std::string_view(direction)));
    }
    auto resolved = resolve_unique(ctx, locator);
    if(!resolved) {
        return std::unexpected(resolved.error());
    }
    TypeHierarchyResult result{.root = graph_entry<GraphEntry>(*resolved)};
    if(direction != "subtypes") {
        collect_targets(ctx, resolved->symbol.hash, RelationKind::Base, result.supertypes);
    }
    if(direction != "supertypes") {
        collect_targets(ctx, resolved->symbol.hash, RelationKind::Derived, result.subtypes);
    }
    return result;
}

}  // namespace clice::query
