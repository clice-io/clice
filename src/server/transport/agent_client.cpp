#include "server/transport/agent_client.h"

#include <algorithm>
#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <vector>

#include "server/protocol/agentic.h"
#include "server/service/query.h"
#include "server/transport/master_server.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/meta/enum.h"
#include "llvm/ADT/DenseSet.h"

namespace clice {

using kota::ipc::RequestResult;
using RequestContext = kota::ipc::JsonPeer::RequestContext;
namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

static std::string_view symbol_kind_name(SymbolKind kind) {
    constexpr auto names = kota::meta::reflection<SymbolKind::Kind>::member_names;
    auto idx = static_cast<std::size_t>(kind.value());
    if(idx < names.size())
        return names[idx];
    return "Unknown";
}

/// Resolve a locator and require a unique match: no candidate is "symbol not
/// found", several candidates ask the client to disambiguate via symbolId.
static std::expected<IndexQuery::Located, kota::ipc::Error>
    resolve_unique(const agentic::ReadSymbolParams& loc, const IndexQuery& query) {
    auto candidates = query.locate(loc);
    if(candidates.empty())
        return std::unexpected(kota::ipc::Error{"symbol not found"});
    if(candidates.size() > 1) {
        return std::unexpected(
            kota::ipc::Error{std::format("ambiguous: {} candidates, use symbolId to disambiguate",
                                         candidates.size())});
    }
    return std::move(candidates[0]);
}

/// The 1-based lines a site spans, as agents read files; nullopt when its
/// bytes fall outside the source's text.
struct AgentLines {
    int start;
    int end;
};

static std::optional<AgentLines> agent_lines(const Site& site) {
    auto range = site.coords.to_range(site.range.begin, site.range.end);
    if(!range)
        return std::nullopt;
    return AgentLines{.start = static_cast<int>(range->start.line) + 1,
                      .end = static_cast<int>(range->end.line) + 1};
}

AgentClient::AgentClient(MasterServer& server, kota::ipc::JsonPeer& peer) :
    server(server), peer(peer) {
    using namespace agentic;

    auto& srv = this->server;

    peer.on_request(
        [&srv](RequestContext&,
               const CompileCommandParams& params) -> RequestResult<CompileCommandParams> {
            srv.pool.foreground_pulse();
            std::string directory;
            std::vector<std::string> arguments;
            // Editor semantics: an agent asks what the user's file compiles
            // as — pins and header context included, never the background
            // tier ladder (which offers a header nothing but fallback).
            srv.contexts.resolve_command(params.path, directory, arguments, ContextUse::Editor);

            co_return CompileCommandResult{
                .file = params.path,
                .directory = std::move(directory),
                .arguments = std::move(arguments),
            };
        });

    peer.on_request([&srv](RequestContext&,
                           const ProjectFilesParams& params) -> RequestResult<ProjectFilesParams> {
        srv.pool.foreground_pulse();
        auto& ws = srv.workspace;
        auto filter = params.filter.value_or("all");

        ProjectFilesResult result;
        llvm::DenseSet<Fid> seen;

        for(auto& entry: ws.cdb.entries()) {
            auto file_path = ws.file_table.resolve(entry.file);
            if(file_path.empty())
                continue;

            auto path_id = entry.file;
            if(!seen.insert(path_id).second)
                continue;

            std::string kind_str;
            auto mod_it = ws.path_to_module.find(path_id);
            if(mod_it != ws.path_to_module.end()) {
                kind_str = "module";
            } else {
                auto ext = llvm::sys::path::extension(file_path);
                if(ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh")
                    kind_str = "header";
                else
                    kind_str = "source";
            }

            if(filter != "all" && filter != kind_str)
                continue;

            FileInfo fi;
            fi.path = file_path.str();
            fi.kind = std::move(kind_str);
            if(mod_it != ws.path_to_module.end())
                fi.module_name = mod_it->second;
            result.files.push_back(std::move(fi));
        }

        if(filter == "all" || filter == "header") {
            for(auto& [path_id, shard]: ws.shards) {
                if(seen.contains(path_id))
                    continue;
                auto path_str = ws.file_table.resolve(path_id);
                auto ext = llvm::sys::path::extension(path_str);
                if(ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh") {
                    seen.insert(path_id);
                    result.files.push_back(FileInfo{
                        .path = path_str.str(),
                        .kind = "header",
                    });
                }
            }
        }

        result.total = static_cast<int>(result.files.size());
        co_return result;
    });

    peer.on_request(
        [&srv](RequestContext&, const FileDepsParams& params) -> RequestResult<FileDepsParams> {
            srv.pool.foreground_pulse();
            auto& ws = srv.workspace;
            // find() applies the canonical spelling; a native uppercase-drive
            // path from an agentic client must hit the same ID.
            auto pool_id = ws.file_table.find(params.path);
            if(!pool_id)
                co_return FileDepsResult{.file = params.path};
            auto path_id = *pool_id;
            auto direction = params.direction.value_or("both");
            auto max_depth = params.depth.value_or(1);

            FileDepsResult result;
            result.file = params.path;

            if(direction == "includes" || direction == "both") {
                auto includes = ws.dep_graph.get_all_includes(path_id);
                for(auto inc_id: includes) {
                    auto inc_path = ws.file_table.resolve(inc_id);
                    result.includes.push_back(DepEntry{.path = inc_path.str(), .depth = 1});
                }

                if(max_depth == 0 || max_depth > 1) {
                    llvm::DenseSet<Fid> visited;
                    visited.insert(path_id);
                    for(auto& dep: result.includes)
                        visited.insert(ws.file_table.intern(dep.path));

                    for(std::size_t i = 0; i < result.includes.size(); ++i) {
                        if(max_depth > 0 && result.includes[i].depth >= max_depth)
                            continue;
                        auto dep_id = ws.file_table.intern(result.includes[i].path);
                        auto sub = ws.dep_graph.get_all_includes(dep_id);
                        for(auto sub_id: sub) {
                            if(!visited.insert(sub_id).second)
                                continue;
                            auto sub_path = ws.file_table.resolve(sub_id);
                            result.includes.push_back(DepEntry{
                                .path = sub_path.str(),
                                .depth = result.includes[i].depth + 1,
                            });
                        }
                    }
                }
            }

            if(direction == "includers" || direction == "both") {
                auto includers = ws.dep_graph.get_includers(path_id);
                for(auto inc_id: includers) {
                    auto inc_path = ws.file_table.resolve(inc_id);
                    result.includers.push_back(DepEntry{.path = inc_path.str(), .depth = 1});
                }

                if(max_depth == 0 || max_depth > 1) {
                    llvm::DenseSet<Fid> visited;
                    visited.insert(path_id);
                    for(auto& dep: result.includers) {
                        if(auto id = ws.file_table.find(dep.path))
                            visited.insert(*id);
                    }

                    for(std::size_t i = 0; i < result.includers.size(); ++i) {
                        if(max_depth > 0 && result.includers[i].depth >= max_depth)
                            continue;
                        auto dep_id = ws.file_table.find(result.includers[i].path);
                        if(!dep_id)
                            continue;
                        auto sub = ws.dep_graph.get_includers(*dep_id);
                        for(auto sub_id: sub) {
                            if(!visited.insert(sub_id).second)
                                continue;
                            auto sub_path = ws.file_table.resolve(sub_id);
                            result.includers.push_back(DepEntry{
                                .path = sub_path.str(),
                                .depth = result.includers[i].depth + 1,
                            });
                        }
                    }
                }
            }

            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&,
               const ImpactAnalysisParams& params) -> RequestResult<ImpactAnalysisParams> {
            srv.pool.foreground_pulse();
            auto& ws = srv.workspace;
            auto pool_id = ws.file_table.find(params.path);
            if(!pool_id)
                co_return ImpactAnalysisResult{};
            auto path_id = *pool_id;

            ImpactAnalysisResult result;

            auto direct_includers = ws.dep_graph.get_includers(path_id);
            for(auto inc_id: direct_includers) {
                result.direct_dependents.push_back(ws.file_table.resolve(inc_id).str());
            }

            auto hosts = ws.dep_graph.find_host_sources(path_id);
            llvm::DenseSet<Fid> seen;
            seen.insert(path_id);
            for(auto inc_id: direct_includers)
                seen.insert(inc_id);
            for(auto host_id: hosts) {
                if(seen.insert(host_id).second)
                    result.transitive_dependents.push_back(ws.file_table.resolve(host_id).str());
            }

            for(auto host_id: hosts) {
                auto it = ws.path_to_module.find(host_id);
                if(it != ws.path_to_module.end())
                    result.affected_modules.push_back(it->second);
            }
            auto mod_it = ws.path_to_module.find(path_id);
            if(mod_it != ws.path_to_module.end())
                result.affected_modules.push_back(mod_it->second);

            co_return result;
        });

    peer.on_request([&srv](RequestContext&,
                           const SymbolSearchParams& params) -> RequestResult<SymbolSearchParams> {
        srv.pool.foreground_pulse();
        srv.on_agentic_query();
        auto max = params.max_results.value_or(100);
        std::string query_lower = llvm::StringRef(params.query).lower();

        SymbolSearchResult result;
        for(auto& [hash, symbol]: srv.workspace.project_index.symbols) {
            if(static_cast<int>(result.symbols.size()) >= max)
                break;
            if(symbol.name.empty())
                continue;
            if(!query_lower.empty() &&
               llvm::StringRef(symbol.name).lower().find(query_lower) == std::string::npos)
                continue;
            if(params.kind_filter.has_value()) {
                auto kind_name = std::string(symbol_kind_name(symbol.kind));
                auto& filter = *params.kind_filter;
                if(std::ranges::find(filter, kind_name) == filter.end())
                    continue;
            }
            auto site = srv.agent_query.first_site(hash, RelationKind::Definition);
            if(!site)
                continue;
            auto lines = agent_lines(*site);
            if(!lines)
                continue;
            result.symbols.push_back(SymbolEntry{
                .name = symbol.name,
                .kind = std::string(symbol_kind_name(symbol.kind)),
                .file = std::string(site->path),
                .line = lines->start,
                .symbol_id = hash,
            });
        }

        co_return result;
    });

    peer.on_request(
        [&srv](RequestContext&, const ReadSymbolParams& params) -> RequestResult<ReadSymbolParams> {
            srv.pool.foreground_pulse();
            srv.on_agentic_query();
            auto resolved = resolve_unique(params, srv.agent_query);
            if(!resolved)
                co_return kota::outcome_error(std::move(resolved.error()));

            auto& rs = *resolved;
            auto definition = srv.agent_query.definition_text(rs.symbol.hash);
            if(!definition)
                co_return kota::outcome_error(kota::ipc::Error{"definition not found"});
            auto lines = agent_lines(definition->extent);
            if(!lines)
                co_return kota::outcome_error(kota::ipc::Error{"definition not found"});

            co_return ReadSymbolResult{
                .name = rs.symbol.name,
                .kind = std::string(symbol_kind_name(rs.symbol.kind)),
                .file = std::string(definition->extent.path),
                .start_line = lines->start,
                .end_line = lines->end,
                .text = std::move(definition->text),
                .symbol_id = rs.symbol.hash,
            };
        });

    peer.on_request(
        [&srv](RequestContext&,
               const DocumentSymbolsParams& params) -> RequestResult<DocumentSymbolsParams> {
            srv.pool.foreground_pulse();
            srv.on_agentic_query();
            auto is_document_level = [](SymbolKind kind) {
                return kind == SymbolKind::Namespace || kind == SymbolKind::Class ||
                       kind == SymbolKind::Struct || kind == SymbolKind::Union ||
                       kind == SymbolKind::Enum || kind == SymbolKind::Type ||
                       kind == SymbolKind::Field || kind == SymbolKind::EnumMember ||
                       kind == SymbolKind::Function || kind == SymbolKind::Method ||
                       kind == SymbolKind::Variable || kind == SymbolKind::Macro ||
                       kind == SymbolKind::Concept || kind == SymbolKind::Module ||
                       kind == SymbolKind::Operator || kind == SymbolKind::Attribute;
            };

            DocumentSymbolsResult result;

            auto path_id = srv.workspace.file_table.find(params.path);
            if(!path_id)
                co_return result;

            // The same serving gate every other agentic lookup applies
            // (freshness contract, clause 2): a shard whose file changed
            // on disk serves nothing until its reindex lands.
            for(auto& located: srv.agent_query.definitions_in(*path_id)) {
                if(!is_document_level(located.symbol.kind))
                    continue;
                auto lines = agent_lines(located.site);
                if(!lines)
                    continue;
                result.symbols.push_back(DocumentSymbolEntry{
                    .name = located.symbol.name,
                    .kind = std::string(symbol_kind_name(located.symbol.kind)),
                    .start_line = lines->start,
                    .end_line = lines->end,
                    .symbol_id = located.symbol.hash,
                });
            }

            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&, const DefinitionParams& params) -> RequestResult<DefinitionParams> {
            srv.pool.foreground_pulse();
            srv.on_agentic_query();
            auto resolved = resolve_unique(
                ReadSymbolParams{params.name, params.path, params.line, params.symbol_id},
                srv.agent_query);
            if(!resolved)
                co_return kota::outcome_error(std::move(resolved.error()));

            auto& rs = *resolved;

            DefinitionResult result;
            result.name = rs.symbol.name;
            result.kind = std::string(symbol_kind_name(rs.symbol.kind));
            result.symbol_id = rs.symbol.hash;

            if(auto definition = srv.agent_query.definition_text(rs.symbol.hash)) {
                if(auto lines = agent_lines(definition->extent)) {
                    result.definition = LocationEntry{
                        .file = std::string(definition->extent.path),
                        .start_line = lines->start,
                        .end_line = lines->end,
                        .text = std::move(definition->text),
                    };
                }
            }

            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&, const ReferencesParams& params) -> RequestResult<ReferencesParams> {
            srv.pool.foreground_pulse();
            srv.on_agentic_query();
            auto resolved = resolve_unique(
                ReadSymbolParams{params.name, params.path, params.line, params.symbol_id},
                srv.agent_query);
            if(!resolved)
                co_return kota::outcome_error(std::move(resolved.error()));

            auto& rs = *resolved;

            ReferencesResult result;
            result.name = rs.symbol.name;
            result.kind = std::string(symbol_kind_name(rs.symbol.kind));
            result.symbol_id = rs.symbol.hash;

            auto collect = [&](RelationKind kind) {
                for(auto& site: srv.agent_query.sites(rs.symbol.hash, kind)) {
                    auto lines = agent_lines(site);
                    if(!lines)
                        continue;
                    result.references.push_back(ReferenceEntry{
                        .file = std::string(site.path),
                        .line = lines->start,
                        .context = srv.agent_query.context_line(site),
                    });
                }
            };
            collect(RelationKind::Reference);
            if(params.include_declaration.value_or(false)) {
                collect(RelationKind::Declaration);
                collect(RelationKind::Definition);
            }

            result.total = static_cast<int>(result.references.size());
            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&, const CallGraphParams& params) -> RequestResult<CallGraphParams> {
            srv.pool.foreground_pulse();
            srv.on_agentic_query();
            auto resolved = resolve_unique(
                ReadSymbolParams{params.name, params.path, params.line, params.symbol_id},
                srv.agent_query);
            if(!resolved)
                co_return kota::outcome_error(std::move(resolved.error()));

            auto& rs = *resolved;
            auto direction = params.direction.value_or("both");

            auto root_lines = agent_lines(rs.site);
            CallGraphResult result;
            result.root = CallGraphEntry{
                .name = rs.symbol.name,
                .kind = std::string(symbol_kind_name(rs.symbol.kind)),
                .file = std::string(rs.site.path),
                .line = root_lines ? root_lines->start : 0,
                .symbol_id = rs.symbol.hash,
            };

            auto collect = [&](RelationKind kind, std::vector<CallGraphEntry>& into) {
                for(auto& group: srv.agent_query.grouped(rs.symbol.hash, kind)) {
                    auto located = srv.agent_query.resolve(group.symbol);
                    if(!located)
                        continue;
                    auto lines = agent_lines(located->site);
                    if(!lines)
                        continue;
                    into.push_back(CallGraphEntry{
                        .name = located->symbol.name,
                        .kind = std::string(symbol_kind_name(located->symbol.kind)),
                        .file = std::string(located->site.path),
                        .line = lines->start,
                        .symbol_id = group.symbol,
                    });
                }
            };
            if(direction == "callers" || direction == "both") {
                collect(RelationKind::Caller, result.callers);
            }
            if(direction == "callees" || direction == "both") {
                collect(RelationKind::Callee, result.callees);
            }

            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&,
               const TypeHierarchyParams& params) -> RequestResult<TypeHierarchyParams> {
            srv.pool.foreground_pulse();
            srv.on_agentic_query();
            auto resolved = resolve_unique(
                ReadSymbolParams{params.name, params.path, params.line, params.symbol_id},
                srv.agent_query);
            if(!resolved)
                co_return kota::outcome_error(std::move(resolved.error()));

            auto& rs = *resolved;
            auto direction = params.direction.value_or("both");

            auto root_lines = agent_lines(rs.site);
            TypeHierarchyResult result;
            result.root = TypeHierarchyEntry{
                .name = rs.symbol.name,
                .kind = std::string(symbol_kind_name(rs.symbol.kind)),
                .file = std::string(rs.site.path),
                .line = root_lines ? root_lines->start : 0,
                .symbol_id = rs.symbol.hash,
            };

            auto collect = [&](RelationKind kind, std::vector<TypeHierarchyEntry>& into) {
                for(auto target: srv.agent_query.targets(rs.symbol.hash, kind)) {
                    auto located = srv.agent_query.resolve(target);
                    if(!located)
                        continue;
                    auto lines = agent_lines(located->site);
                    if(!lines)
                        continue;
                    into.push_back(TypeHierarchyEntry{
                        .name = located->symbol.name,
                        .kind = std::string(symbol_kind_name(located->symbol.kind)),
                        .file = std::string(located->site.path),
                        .line = lines->start,
                        .symbol_id = target,
                    });
                }
            };
            if(direction == "supertypes" || direction == "both") {
                collect(RelationKind::Base, result.supertypes);
            }
            if(direction == "subtypes" || direction == "both") {
                collect(RelationKind::Derived, result.subtypes);
            }

            co_return result;
        });

    peer.on_request([&srv](RequestContext&, const StatusParams&) -> RequestResult<StatusParams> {
        srv.pool.foreground_pulse();
        // The progress numbers describe the current round — or the last
        // one, retained after it ends; the live queue is compacted between
        // rounds and would read as "nothing was ever indexed".
        auto& progress = srv.pump.progress();
        StatusResult result;
        result.idle = srv.pump.is_idle();
        result.pending = static_cast<int>(srv.pump.pending_files());
        result.total = static_cast<int>(progress.total);
        result.indexed = static_cast<int>(progress.completed);
        co_return result;
    });

    peer.on_notification([&srv](const ShutdownParams&) {
        LOG_INFO("agentic/shutdown received, shutting down");
        srv.schedule_shutdown();
    });
}

}  // namespace clice
