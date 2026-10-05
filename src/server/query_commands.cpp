#include "server/query_commands.h"

#include <format>

#include "command/toolchain.h"
#include "index/serialization.h"
#include "project/build.h"
#include "vfs/file_system.h"
#include "vfs/path.h"

#include "kota/meta/enum.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Path.h"

namespace clice::query {

namespace {

std::string kind_name(SymbolKind kind) {
    return std::string(kota::meta::enum_name(static_cast<SymbolKind::Kind>(kind), "Unknown"));
}

/// The 1-based lines a site spans, as the answers spell positions.
struct Lines {
    int start;
    int end;
};

Lines lines_of(const index::Site& site) {
    return {.start = static_cast<int>(site.begin.line) + 1,
            .end = static_cast<int>(site.end.line) + 1};
}

/// The file a path names in the index. An error for a path that is not a
/// file; nullopt (noted as unindexed) for one the index has no rows for.
Outcome<std::optional<Fid>> indexed_file(Context& ctx, const Spelling& path) {
    if(!vfs::is_file(path)) {
        return std::unexpected(std::format("no such file: {}", path));
    }
    // Interning only names the file; whether the index holds rows for
    // it is the shard fetch's answer.
    auto file = ctx.project.file_table.intern(path);
    if(!ctx.project.project_index.shard(file)) {
        ctx.unindexed.emplace_back(ctx.project.file_table.display(file));
        return std::nullopt;
    }
    return file;
}

/// Anchor a query's paths in the workspace: a rooted path filter names
/// its file by identity, however the command line spelled it, and the
/// place's path becomes absolute the way the command's own path arguments
/// do. False when the place's file is not indexed, which the caller
/// answers as not found with the path noted.
Outcome<bool> anchor_paths(Context& ctx, index::SymbolQuery& query) {
    for(auto& wanted: query.paths) {
        if(path::is_absolute(wanted)) {
            wanted = CanonicalPath(Spelling::absolute(wanted)).str();
        }
    }
    if(!query.position) {
        return true;
    }
    auto& place = *query.position;
    Spelling spelled(place.path, Spelling(ctx.project.config.workspace_root));
    place.path = spelled.str();
    auto file = indexed_file(ctx, spelled);
    if(!file) {
        return std::unexpected(file.error());
    }
    return file->has_value();
}

/// Resolve a locator to exactly one symbol: no candidate is an unknown
/// symbol, several ask the caller to disambiguate by id. A locator naming
/// a path the index has no rows for answers as unknown and notes the path.
Outcome<index::IndexQuery::Located> resolve_unique(Context& ctx, index::SymbolQuery query) {
    auto anchored = anchor_paths(ctx, query);
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
                                  index::symbol_id(candidate.symbol.hash));
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
std::vector<DepEntry> collect_deps(Project& ws,
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
            entries.push_back({.path = ws.file_table.display(next), .depth = depth + 1});
        }
    }
    return entries;
}

template <typename Entry>
Entry graph_entry(const index::IndexQuery::Located& located) {
    return {
        .name = located.symbol.display_name(),
        .kind = kind_name(located.symbol.kind),
        .file = std::string(located.site.path),
        .line = lines_of(located.site).start,
        .symbol_id = index::symbol_id(located.symbol.hash),
    };
}

std::vector<GraphEntry> graph_entries(llvm::ArrayRef<index::IndexQuery::Located> symbols) {
    std::vector<GraphEntry> entries;
    entries.reserve(symbols.size());
    for(auto& located: symbols) {
        entries.push_back(graph_entry<GraphEntry>(located));
    }
    return entries;
}

std::vector<GraphEntry> graph_entries(llvm::ArrayRef<index::IndexQuery::Edge> edges) {
    std::vector<GraphEntry> entries;
    entries.reserve(edges.size());
    for(auto& edge: edges) {
        entries.push_back(graph_entry<GraphEntry>(edge.symbol));
    }
    return entries;
}

}  // namespace

Outcome<CompileCommandResult> compile_command(Context& ctx, const Spelling& path) {
    if(!vfs::is_file(path)) {
        return std::unexpected(std::format("no such file: {}", path));
    }
    // The editor compiles such a header under a synthesized preamble, a
    // cache artifact a read-only reader cannot produce; the host's bare
    // command would be a different compile.
    auto needs_context = [&](Fid file) {
        auto* choice = ctx.contexts.selection(file);
        return ctx.contexts.commands.header_mode(file) == HeaderMode::NeedsContext ||
               (choice && choice->host_path_id.valid() && choice->occurrence.has_value());
    };
    auto file = ctx.project.file_table.intern(path);
    if(needs_context(file)) {
        return std::unexpected(std::format(
            "{} compiles only under a synthesized header context, which needs an editor session",
            path));
    }
    auto& files = ctx.project.file_table;
    CompileCommandResult result{.file = files.display(file)};
    auto resolution = ctx.contexts.resolve_command(file, result.directory, result.arguments);
    result.directory = files.display(CanonicalPath(Spelling::absolute(result.directory)));
    auto& ref = resolution.ref;
    if(auto resolved = ctx.project.cdb.toolchain().resolve(ref.config, ref.input); !resolved) {
        result.toolchain_error = std::move(resolved.error());
    }
    switch(resolution.source) {
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
    auto& ws = ctx.project;
    ProjectFilesResult result;
    llvm::DenseSet<Fid> seen;
    for(auto member: ws.build.members()) {
        auto file_path = ws.file_table.resolve(member);
        if(file_path.empty() || !seen.insert(member).second) {
            continue;
        }
        auto module_name = ws.dep_graph.module_of(member);
        llvm::StringRef kind = !module_name.empty()        ? "module"
                               : is_header_path(file_path) ? "header"
                                                           : "source";
        if(filter != "all" && filter != kind) {
            continue;
        }
        FileInfo info{.path = ws.file_table.display(member), .kind = kind.str()};
        if(!module_name.empty()) {
            info.module_name = module_name.str();
        }
        result.files.push_back(std::move(info));
    }
    if(filter == "all" || filter == "header") {
        for(auto path_id: llvm::make_first_range(ws.project_index.shards)) {
            auto path = ws.file_table.resolve(path_id);
            if(!seen.contains(path_id) && is_header_path(path)) {
                seen.insert(path_id);
                result.files.push_back({.path = ws.file_table.display(path_id), .kind = "header"});
            }
        }
    }
    result.total = static_cast<int>(result.files.size());
    return result;
}

Outcome<FileDepsResult>
    file_deps(Context& ctx, const Spelling& path, llvm::StringRef direction, int depth) {
    if(!llvm::is_contained<llvm::StringRef>({"includes", "includers", "both"}, direction)) {
        return std::unexpected(
            std::format("invalid direction '{}': expected includes, includers or both",
                        std::string_view(direction)));
    }
    if(depth < 0) {
        return std::unexpected("depth must not be negative");
    }
    if(!vfs::is_file(path)) {
        return std::unexpected(std::format("no such file: {}", path));
    }
    auto& ws = ctx.project;
    FileDepsResult result{.file = path.str()};
    auto file = ws.file_table.find(path);
    if(!file) {
        return result;
    }
    result.file = ws.file_table.display(*file);
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

Outcome<ImpactAnalysisResult> impact_analysis(Context& ctx, const Spelling& path) {
    if(!vfs::is_file(path)) {
        return std::unexpected(std::format("no such file: {}", path));
    }
    auto& ws = ctx.project;
    ImpactAnalysisResult result;
    auto file = ws.file_table.find(path);
    if(!file) {
        return result;
    }
    llvm::DenseSet<Fid> seen{*file};
    for(auto dependent: llvm::concat<const Fid>(ws.dep_graph.get_includers(*file),
                                                ws.dep_graph.get_forcing_units(*file))) {
        if(seen.insert(dependent).second) {
            result.direct_dependents.push_back(ws.file_table.display(dependent));
        }
    }
    auto readers = ws.dep_graph.find_readers(*file);
    for(auto reader: readers) {
        if(seen.insert(reader).second) {
            result.transitive_dependents.push_back(ws.file_table.display(reader));
        }
    }
    for(auto reader: readers) {
        auto module_name = ws.dep_graph.module_of(reader);
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
    auto anchored = anchor_paths(ctx, *query);
    if(!anchored) {
        return std::unexpected(anchored.error());
    }
    if(!*anchored) {
        return result;
    }
    auto located = query->by_pattern() ? ctx.query.search(*query, limit) : ctx.query.locate(*query);
    // A locator names its symbols outright; the filters still apply.
    if(!query->by_pattern()) {
        llvm::erase_if(located, [&](const index::IndexQuery::Located& hit) {
            if(!query->kinds.empty() && !llvm::is_contained(query->kinds, hit.symbol.kind)) {
                return true;
            }
            return !query->paths.empty() &&
                   llvm::none_of(query->paths, [&](const std::string& wanted) {
                       return hit.site.file.valid() &&
                              index::path_matches(wanted,
                                                  ctx.project.file_table.resolve(hit.site.file));
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

Outcome<ReadSymbolResult> read_symbol(Context& ctx, index::SymbolQuery locator) {
    auto resolved = resolve_unique(ctx, std::move(locator));
    if(!resolved) {
        return std::unexpected(resolved.error());
    }
    auto definition = ctx.query.definition_text(resolved->symbol.hash, resolved->site.file);
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
        .symbol_id = index::symbol_id(resolved->symbol.hash),
    };
}

Outcome<DocumentSymbolsResult> document_symbols(Context& ctx, const Spelling& path) {
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
            .symbol_id = index::symbol_id(located.symbol.hash),
        });
    }
    return result;
}

Outcome<DefinitionResult> definition(Context& ctx, index::SymbolQuery locator) {
    auto resolved = resolve_unique(ctx, std::move(locator));
    if(!resolved) {
        return std::unexpected(resolved.error());
    }
    DefinitionResult result{
        .name = resolved->symbol.display_name(),
        .kind = kind_name(resolved->symbol.kind),
        .symbol_id = index::symbol_id(resolved->symbol.hash),
    };
    if(auto definition = ctx.query.definition_text(resolved->symbol.hash, resolved->site.file)) {
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
                                     index::SymbolQuery locator,
                                     bool include_declaration) {
    auto resolved = resolve_unique(ctx, std::move(locator));
    if(!resolved) {
        return std::unexpected(resolved.error());
    }
    ReferencesResult result{
        .name = resolved->symbol.display_name(),
        .kind = kind_name(resolved->symbol.kind),
        .symbol_id = index::symbol_id(resolved->symbol.hash),
    };
    index::IndexQuery::Cursor cursor{.symbols = {resolved->symbol.hash}, .site = resolved->site};
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

bool units_pending(Project& project) {
    return llvm::any_of(project.build.members(), [&](Fid unit) {
        return project.build.indexed(project.file_table.resolve(unit)) &&
               !project.project_index.tu_manifest(unit);
    });
}

Outcome<PlannedRename> rename(Context& ctx, index::SymbolQuery locator, llvm::StringRef new_name) {
    auto resolved = resolve_unique(ctx, std::move(locator));
    if(!resolved) {
        return std::unexpected(resolved.error());
    }
    auto target = index::rename_target(ctx.query, *resolved);
    if(!target) {
        return std::unexpected(target.error());
    }
    auto& symbol = target->symbol.symbol;

    auto& config = ctx.project.config;
    CanonicalRef root = config.workspace_root;
    CanonicalPath cache_dir;
    if(!config.project.cache_dir.empty()) {
        cache_dir = CanonicalPath(Spelling::absolute(config.project.cache_dir));
    }
    std::vector<std::string> sources;
    for(auto& source: workspace_sources(root, cache_dir)) {
        sources.push_back(source.str());
    }
    auto editable = [&](llvm::StringRef path) {
        return workspace_file(root, cache_dir, CanonicalPath(Spelling::absolute(path)));
    };
    auto read = [&](llvm::StringRef path) -> std::optional<index::SweptText> {
        auto text = vfs::read(path);
        if(!text) {
            return std::nullopt;
        }
        return index::sweep_text((*text)->getBuffer().str(), symbol.name, new_name);
    };
    auto plan = index::plan_rename(ctx.query,
                                   ctx.project.file_table,
                                   *target,
                                   new_name,
                                   {.files = sources,
                                    .editable = editable,
                                    .read = read,
                                    .units_pending = units_pending(ctx.project)});
    RenameResult result{
        .name = symbol.display_name(),
        .kind = kind_name(symbol.kind),
        .symbol_id = index::symbol_id(symbol.hash),
        .new_name = new_name.str(),
        .conflicts = plan.conflicts,
        .warnings = plan.warnings,
        .stale = plan.stale,
    };
    for(auto& edit: plan.edits) {
        if(result.files.empty() || result.files.back().file != edit.site.path) {
            result.files.push_back({.file = edit.site.path});
        }
        result.files.back().edits.push_back({
            .line = static_cast<int>(edit.site.begin.line) + 1,
            .column = static_cast<int>(edit.site.begin.column) + 1,
            .heuristic = edit.heuristic,
        });
    }
    for(auto& note: plan.unconfirmed) {
        result.unconfirmed.push_back({
            .file = note.site.path,
            .line = static_cast<int>(note.site.begin.line) + 1,
            .column = static_cast<int>(note.site.begin.column) + 1,
            .reason = note.reason,
            .text = note.line,
        });
    }
    return PlannedRename{.result = std::move(result), .plan = std::move(plan)};
}

Outcome<CallGraphResult> call_graph(Context& ctx,
                                    index::SymbolQuery locator,
                                    llvm::StringRef direction) {
    if(!llvm::is_contained<llvm::StringRef>({"callers", "callees", "both"}, direction)) {
        return std::unexpected(
            std::format("invalid direction '{}': expected callers, callees or both",
                        std::string_view(direction)));
    }
    auto resolved = resolve_unique(ctx, std::move(locator));
    if(!resolved) {
        return std::unexpected(resolved.error());
    }
    auto graph = ctx.query.call_graph(
        resolved->symbol.hash,
        resolved->site.file,
        {.callers = direction != "callees", .callees = direction != "callers"});
    return CallGraphResult{
        .root = graph_entry<GraphEntry>(*resolved),
        .callers = graph_entries(graph.callers),
        .callees = graph_entries(graph.callees),
    };
}

Outcome<TypeHierarchyResult> type_hierarchy(Context& ctx,
                                            index::SymbolQuery locator,
                                            llvm::StringRef direction) {
    if(!llvm::is_contained<llvm::StringRef>({"supertypes", "subtypes", "both"}, direction)) {
        return std::unexpected(
            std::format("invalid direction '{}': expected supertypes, subtypes or both",
                        std::string_view(direction)));
    }
    auto resolved = resolve_unique(ctx, std::move(locator));
    if(!resolved) {
        return std::unexpected(resolved.error());
    }
    auto hierarchy = ctx.query.type_hierarchy(
        resolved->symbol.hash,
        resolved->site.file,
        {.supertypes = direction != "subtypes", .subtypes = direction != "supertypes"});
    return TypeHierarchyResult{
        .root = graph_entry<GraphEntry>(*resolved),
        .supertypes = graph_entries(hierarchy.supertypes),
        .subtypes = graph_entries(hierarchy.subtypes),
    };
}

}  // namespace clice::query
