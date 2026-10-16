module;

#include "modules/prelude.h"

module clice;

import :command.toolchain;
import :feature.feature;
import :index.serialization;
import :project.build;
import :server.query_commands;
import :support.markup;
import :syntax.documentation;
import :vfs.file_system;
import :vfs.path;

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
/// file; nullopt (noted as unindexed) for one the index has no shard for.
Outcome<std::optional<Fid>> indexed_file(Context& ctx, const Spelling& path) {
    if(!vfs::is_file(path)) {
        return std::unexpected(std::format("no such file: {}", path));
    }
    // Interning only names the file; whether the index holds a shard for
    // it is the shard fetch's answer.
    auto file = ctx.project.file_table.intern(path);
    if(!ctx.project.project_index.shard(file)) {
        ctx.unindexed.emplace_back(ctx.project.file_table.display(file));
        return std::nullopt;
    }
    return file;
}

/// Anchor a query's paths in the workspace. The file of a place, and the
/// one holding an id's entries, is one file, workspace-relative or absolute
/// as the command's own path arguments are; false when the index holds no
/// shard for it, which the caller answers as not found with the file
/// noted. A path filter spelling a path names its file or directory by
/// identity, a relative one from the workspace root.
Outcome<bool> anchor_paths(Context& ctx, index::SymbolQuery& query) {
    auto anchor = [&](std::string& path) -> Outcome<bool> {
        Spelling spelled(path, Spelling(ctx.project.config.workspace_root));
        auto file = indexed_file(ctx, spelled);
        if(!file) {
            return std::unexpected(file.error());
        }
        path = spelled.str();
        return file->has_value();
    };
    if(query.handle) {
        for(auto& path: query.paths) {
            if(auto anchored = anchor(path); !anchored || !*anchored) {
                return anchored;
            }
        }
        return true;
    }
    query.anchor(Spelling(ctx.project.config.workspace_root));
    return query.position ? anchor(query.position->path) : true;
}

/// The locator an answer names a symbol by: one only some files see is
/// anchored at the file of its site, which holds its entries.
std::string symbol_of(const index::IndexQuery::Located& located) {
    bool external = located.symbol.scope == index::SymbolScope::External;
    return index::symbol_locator(located.symbol.hash,
                                 external ? llvm::StringRef() : located.site.path);
}

/// What the symbol's documentation comment says, as markdown.
std::optional<std::string> documentation_of(Context& ctx,
                                            const index::IndexQuery::Located& located) {
    auto documentation = feature::documentation(ctx.query,
                                                located.symbol.hash,
                                                located.site.file,
                                                ctx.project.config.documentation);
    if(documentation.empty()) {
        return std::nullopt;
    }
    return feature::present(documentation).as_markdown();
}

template <typename Entry>
Entry graph_entry(const index::IndexQuery::Located& located) {
    return {
        .name = located.symbol.display_name(),
        .kind = kind_name(located.symbol.kind),
        .file = std::string(located.site.path),
        .line = lines_of(located.site).start,
        .symbol = symbol_of(located),
    };
}

SymbolEntry symbol_entry(Context& ctx, const index::IndexQuery::Located& located) {
    auto entry = graph_entry<SymbolEntry>(located);
    if(auto container = ctx.query.container_name(located.symbol.hash); !container.empty()) {
        entry.container = std::move(container);
    }
    return entry;
}

/// The symbols a locator names, held to its filters: an id or a place
/// names its symbols outright, which a pattern's lookup filters itself.
/// An id's paths name the file holding its entries, no filter.
std::vector<index::IndexQuery::Located> located_by(Context& ctx, const index::SymbolQuery& query) {
    auto located = ctx.query.locate(query);
    if(query.by_pattern()) {
        return located;
    }
    llvm::erase_if(located, [&](const index::IndexQuery::Located& hit) {
        if(!query.kinds.empty() && !llvm::is_contained(query.kinds, hit.symbol.kind)) {
            return true;
        }
        return !query.handle && !query.paths.empty() &&
               llvm::none_of(query.paths, [&](const std::string& wanted) {
                   return hit.site.file.valid() &&
                          index::path_matches(wanted,
                                              ctx.project.file_table.resolve(hit.site.file));
               });
    });
    return located;
}

/// The symbols a locator names; none is an unknown symbol. A locator
/// naming a file the index has no shard for answers as unknown and notes
/// the file.
Outcome<std::vector<index::IndexQuery::Located>> resolve(Context& ctx, index::SymbolQuery query) {
    auto anchored = anchor_paths(ctx, query);
    if(!anchored) {
        return std::unexpected(anchored.error());
    }
    auto candidates =
        *anchored ? located_by(ctx, query) : std::vector<index::IndexQuery::Located>();
    if(candidates.empty()) {
        return std::unexpected("symbol not found");
    }
    return candidates;
}

/// Several symbols where one is wanted: listed for the caller to pick by
/// locator.
std::unexpected<std::string> ambiguous(Context& ctx,
                                       llvm::ArrayRef<index::IndexQuery::Located> candidates) {
    for(auto& candidate: candidates) {
        ctx.candidates.push_back(symbol_entry(ctx, candidate));
    }
    return std::unexpected(
        std::format("ambiguous: {} symbols match; name one by the `symbol` of a candidate",
                    candidates.size()));
}

/// Resolve a locator to exactly one symbol.
Outcome<index::IndexQuery::Located> resolve_unique(Context& ctx, index::SymbolQuery query) {
    auto candidates = resolve(ctx, std::move(query));
    if(!candidates) {
        return std::unexpected(candidates.error());
    }
    if(candidates->size() > 1) {
        return ambiguous(ctx, *candidates);
    }
    return std::move(candidates->front());
}

/// Hold a name query to the whole name: a near match is no symbol to act
/// on, and a pattern names many. False for a pattern.
bool hold_to_whole_name(index::SymbolQuery& locator) {
    using Mode = index::SymbolQuery::Mode;
    if(!locator.by_pattern() || locator.mode == Mode::Exact) {
        return true;
    }
    if(locator.mode == Mode::Fuzzy) {
        locator.mode = Mode::Exact;
        return true;
    }
    return false;
}

/// Whether the symbols are the overloads of one scope. One of internal
/// linkage shares a scope only within its file — two units' `static`
/// helpers are no overload set.
bool one_overload_set(llvm::ArrayRef<const index::IndexQuery::Located*> symbols) {
    auto& first = *symbols.front();
    auto overloaded = [&](const index::IndexQuery::Located* located) {
        auto& symbol = located->symbol;
        return (symbol.kind == SymbolKind::Function || symbol.kind == SymbolKind::Method ||
                symbol.kind == SymbolKind::Operator) &&
               symbol.parent == first.symbol.parent;
    };
    auto internal = [](const index::IndexQuery::Located* located) {
        return located->symbol.scope != index::SymbolScope::External;
    };
    auto one_file = [&](const index::IndexQuery::Located* located) {
        return located->site.file == first.site.file;
    };
    return llvm::all_of(symbols, overloaded) &&
           (llvm::none_of(symbols, internal) || llvm::all_of(symbols, one_file));
}

/// The rename target of the symbols a locator names: one symbol's, or,
/// for a name the overloads of one scope spell, all of theirs — renamed
/// together, a call a template leaves open between them stays theirs.
Outcome<index::RenameTarget> target_of(Context& ctx, index::SymbolQuery locator) {
    if(!hold_to_whole_name(locator)) {
        return std::unexpected("a rename names its symbol by its whole name, not by a pattern");
    }
    bool by_pattern = locator.by_pattern();
    auto candidates = resolve(ctx, std::move(locator));
    if(!candidates) {
        return std::unexpected(candidates.error());
    }
    std::vector<index::RenameTarget> targets;
    for(auto& candidate: *candidates) {
        auto target = index::rename_target(ctx.query, candidate);
        if(!target) {
            if(candidates->size() == 1) {
                return std::unexpected(target.error());
            }
            return ambiguous(ctx, *candidates);
        }
        auto root = target->symbol.symbol.hash;
        if(llvm::none_of(targets, [&](const index::RenameTarget& known) {
               return known.symbol.symbol.hash == root;
           })) {
            targets.push_back(std::move(*target));
        }
    }
    if(targets.size() > 1) {
        llvm::SmallVector<const index::IndexQuery::Located*> roots;
        for(auto& target: targets) {
            roots.push_back(&target.symbol);
        }
        if(!by_pattern || !one_overload_set(roots)) {
            return ambiguous(ctx, *candidates);
        }
    }
    auto merged = std::move(targets.front());
    for(auto& other: llvm::drop_begin(targets)) {
        llvm::append_range(merged.group, other.group);
    }
    return merged;
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
    auto& files = ctx.project.file_table;
    auto file = files.intern(path);
    CompileCommandResult result{.file = files.display(file)};
    auto resolution = ctx.contexts.resolve_command(file, result.directory, result.arguments);
    // The command names in-memory files the synthesized context serves to
    // an editor compile; the host's bare command would be a different one.
    if(resolution.synthesized) {
        return std::unexpected(std::format(
            "{} compiles only under a synthesized header context, which needs an editor session",
            path));
    }
    result.directory = files.display(CanonicalPath(Spelling::absolute(result.directory)));
    auto& ref = resolution.ref;
    if(auto resolved = ctx.project.cdb.toolchain().resolve(ref.config, ref.input); !resolved) {
        result.toolchain_error = std::move(resolved.error());
    }
    switch(resolution.source) {
        case CommandSource::CDBExact: result.source = "database"; break;
        case CommandSource::IncludeGraph: result.source = "host"; break;
        case CommandSource::Default: result.source = "rule"; break;
        case CommandSource::Inferred:
            result.source = ctx.project.build.unit(file) ? "provisional" : "inferred";
            break;
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
    // An importer reads the files its modules were built from without
    // including them.
    if(auto it = ws.project_index.importers.find(*file); it != ws.project_index.importers.end()) {
        llvm::SmallVector<Fid> importers(it->second.begin(), it->second.end());
        llvm::sort(importers, [&](Fid lhs, Fid rhs) {
            return ws.file_table.resolve(lhs) < ws.file_table.resolve(rhs);
        });
        for(auto importer: importers) {
            if(!llvm::is_contained(readers, importer)) {
                readers.push_back(importer);
            }
        }
    }
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

Outcome<SymbolSearchResult> symbol_search(Context& ctx, llvm::StringRef text, std::size_t limit) {
    auto query = index::SymbolQuery::parse(text);
    if(!query) {
        return std::unexpected(query.error());
    }
    SymbolSearchResult result;
    auto anchored = anchor_paths(ctx, *query);
    if(!anchored) {
        return std::unexpected(anchored.error());
    }
    if(!*anchored) {
        return result;
    }
    auto located = query->by_pattern() ? ctx.query.search(*query, limit) : located_by(ctx, *query);
    if(located.size() > limit) {
        located.resize(limit);
    }
    for(auto& hit: located) {
        result.symbols.push_back(symbol_entry(ctx, hit));
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
        .documentation = documentation_of(ctx, *resolved),
        .text = std::move(definition->text),
        .symbol = symbol_of(*resolved),
    };
}

Outcome<DocumentSymbolsResult> document_symbols(Context& ctx, const Spelling& path) {
    auto file = indexed_file(ctx, path);
    if(!file) {
        return std::unexpected(file.error());
    }
    DocumentSymbolsResult result;
    if(!*file) {
        return result;
    }
    auto source = ctx.query.serving(**file);
    if(!source) {
        return result;
    }
    auto file_path = ctx.project.file_table.resolve(**file);
    auto text = vfs::read(file_path);
    if(!text) {
        return result;
    }
    // The outline the editor shows of a file it has not compiled; a
    // function's locals are no part of it.
    auto entries = feature::extract_index_entries(*source->shard);
    auto resolve = [&](index::SymbolHash hash) {
        return ctx.query.symbol_info(hash, **file);
    };
    auto outline = feature::index_document_symbols((*text)->getBuffer(),
                                                   feature::index_lang_options(file_path, false),
                                                   entries,
                                                   resolve,
                                                   {.locals = false});
    // The entry of a `#define` or a module declaration is where its symbol
    // is declared.
    llvm::DenseMap<std::uint32_t, index::SymbolHash> declared;
    for(const auto& decl: entries.decls) {
        if(auto info = resolve(decl.symbol);
           info && (info->kind == SymbolKind::Macro || info->kind == SymbolKind::Module)) {
            declared.try_emplace(decl.range.begin, decl.symbol);
        }
    }
    auto list = [&](this auto& self, llvm::ArrayRef<feature::DocumentSymbol> symbols) -> void {
        for(const auto& symbol: symbols) {
            auto hash =
                symbol.entity != 0 ? symbol.entity : declared.lookup(symbol.selection_range.begin);
            if(auto info = hash != 0 ? resolve(hash) : std::nullopt) {
                auto extent = source->site(symbol.range);
                auto name = source->site(symbol.selection_range);
                auto lines = lines_of(*extent);
                result.symbols.push_back({
                    .name = symbol.name,
                    .kind = kind_name(symbol.kind),
                    .start_line = lines.start,
                    .end_line = lines.end,
                    .symbol = symbol_of({.symbol = *info, .site = *name, .extent = *extent}),
                });
            }
            self(symbol.children);
        }
    };
    list(outline);
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
        .symbol = symbol_of(*resolved),
        .documentation = documentation_of(ctx, *resolved),
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
        .symbol = symbol_of(*resolved),
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

PlannedRename rename(Context& ctx, llvm::ArrayRef<RenameRequest> requests) {
    PlannedRename planned;
    auto& result = planned.result;
    std::vector<index::Renaming> renamings;
    // The answer's entry of each renaming.
    std::vector<std::size_t> entries;
    llvm::StringSet<> names;
    for(auto& request: requests) {
        auto& entry = result.renames.emplace_back(RenameEntry{
            .line = request.line,
            .locator = request.text,
            .new_name = request.new_name,
        });
        auto target = target_of(ctx, request.locator);
        if(!target) {
            entry.error = std::move(target.error());
            if(!ctx.candidates.empty()) {
                entry.candidates = std::exchange(ctx.candidates, {});
            }
            continue;
        }
        auto& symbol = target->symbol.symbol;
        entry.name = symbol.display_name();
        entry.kind = kind_name(symbol.kind);
        entry.symbol = symbol_of(target->symbol);
        names.insert(symbol.name);
        names.insert(request.new_name);
        entries.push_back(result.renames.size() - 1);
        renamings.push_back({.target = std::move(*target), .new_name = request.new_name});
    }

    auto& config = ctx.project.config;
    CanonicalRef root = config.workspace_root;
    CanonicalPath cache_dir;
    if(!config.project.cache_dir.empty()) {
        cache_dir = CanonicalPath(Spelling::absolute(config.project.cache_dir));
    }
    std::vector<std::string> sources;
    llvm::StringSet<> walked;
    llvm::StringMap<index::ScannedText> scanned;
    auto scan = [&](llvm::StringRef path) -> const index::ScannedText* {
        walked.insert(path);
        auto text = vfs::read(path);
        if(!text) {
            return nullptr;
        }
        auto spelled = index::scan_text((*text)->getBuffer().str(), names);
        return spelled ? &scanned.try_emplace(path, std::move(*spelled)).first->second : nullptr;
    };
    for(auto& source: workspace_sources(root, cache_dir)) {
        scan(sources.emplace_back(source.str()));
    }
    auto editable = [&](llvm::StringRef path) {
        return workspace_file(root, cache_dir, CanonicalPath(Spelling::absolute(path)));
    };
    auto read = [&](llvm::StringRef path) -> const index::ScannedText* {
        if(auto found = scanned.find(path); found != scanned.end()) {
            return &found->second;
        }
        // An edited file the walk's suffixes left out.
        return walked.contains(path) ? nullptr : scan(path);
    };
    planned.plan = index::plan_renames(ctx.query,
                                       ctx.project.file_table,
                                       renamings,
                                       {.files = sources,
                                        .editable = editable,
                                        .read = read,
                                        .units_pending = units_pending(ctx.project)});

    auto& plans = planned.plan.plans;
    std::set<std::string> stale;
    std::set<std::tuple<std::string, std::uint32_t, std::string>> noted;
    for(std::size_t i = 0; i < plans.size(); i += 1) {
        auto& plan = plans[i];
        auto& entry = result.renames[entries[i]];
        entry.edits = static_cast<int>(plan.edits.size());
        entry.conflicts = plan.conflicts;
        entry.warnings = plan.warnings;
        stale.insert(plan.stale.begin(), plan.stale.end());
        for(auto& note: plan.unconfirmed) {
            if(!noted.insert({note.site.path, note.site.range.begin, plan.old_name}).second) {
                continue;
            }
            result.unconfirmed.push_back({
                .file = note.site.path,
                .line = static_cast<int>(note.site.begin.line) + 1,
                .column = static_cast<int>(note.site.begin.column) + 1,
                .name = plan.old_name,
                .reason = note.reason,
                .text = note.line,
            });
        }
    }
    std::ranges::sort(result.unconfirmed,
                      [](const RenameNoteEntry& lhs, const RenameNoteEntry& rhs) {
                          return std::tie(lhs.file, lhs.line, lhs.column, lhs.name) <
                                 std::tie(rhs.file, rhs.line, rhs.column, rhs.name);
                      });
    for(auto& [edit, owner]: planned.plan.edits) {
        auto& site = edit.site;
        if(result.files.empty() || result.files.back().file != site.path) {
            result.files.push_back({.file = site.path});
        }
        result.files.back().edits.push_back({
            .line = static_cast<int>(site.begin.line) + 1,
            .column = static_cast<int>(site.begin.column) + 1,
            .name = plans[owner].old_name,
            .new_name = plans[owner].new_name,
            .heuristic = edit.heuristic,
        });
    }
    result.conflicts = planned.plan.conflicts;
    result.stale.assign(stale.begin(), stale.end());
    return planned;
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

Outcome<std::vector<index::SymbolHash>> declaration(Context& ctx, index::SymbolQuery locator) {
    if(!hold_to_whole_name(locator)) {
        return std::unexpected("a declaration is named by its whole name, not by a pattern");
    }
    bool by_pattern = locator.by_pattern();
    auto candidates = resolve(ctx, std::move(locator));
    if(!candidates) {
        return std::unexpected(candidates.error());
    }
    llvm::erase_if(*candidates, [](const index::IndexQuery::Located& candidate) {
        return candidate.symbol.kind == SymbolKind::Macro;
    });
    if(candidates->empty()) {
        return std::unexpected("the name spells only a macro, not a declaration");
    }
    llvm::SmallVector<const index::IndexQuery::Located*> symbols;
    for(auto& candidate: *candidates) {
        symbols.push_back(&candidate);
    }
    if(symbols.size() > 1 && (!by_pattern || !one_overload_set(symbols))) {
        return ambiguous(ctx, *candidates);
    }
    std::vector<index::SymbolHash> hashes;
    for(auto& candidate: *candidates) {
        hashes.push_back(candidate.symbol.hash);
    }
    return hashes;
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
