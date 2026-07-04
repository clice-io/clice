#include "server/service/agent_client.h"

#include <algorithm>
#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <vector>

#include "server/protocol/agentic.h"
#include "server/service/master_server.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/uri.h"
#include "kota/meta/enum.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

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

struct ResolvedSymbol {
    index::SymbolHash hash = 0;
    std::string name;
    SymbolKind kind;
    std::string file;
    int line = 0;
};

static std::vector<ResolvedSymbol> resolve_locator(const agentic::ReadSymbolParams& loc,
                                                   Workspace& workspace,
                                                   Indexer& indexer) {
    if(loc.symbol_id.has_value() && *loc.symbol_id != 0) {
        auto hash = static_cast<index::SymbolHash>(*loc.symbol_id);
        std::string name;
        SymbolKind kind;
        if(!indexer.find_symbol_info(hash, name, kind))
            return {};
        auto def_loc = indexer.find_definition_location(hash);
        if(!def_loc)
            return {};
        auto file = uri_to_path(def_loc->uri);
        int line_num = static_cast<int>(def_loc->range.start.line) + 1;
        return {
            {hash, std::move(name), kind, std::move(file), line_num}
        };
    }

    if(loc.name.has_value() && !loc.name->empty()) {
        std::string query_lower = llvm::StringRef(*loc.name).lower();
        std::vector<ResolvedSymbol> candidates;
        std::vector<ResolvedSymbol> exact_matches;
        llvm::DenseSet<index::SymbolHash> seen;

        auto try_symbol = [&](index::SymbolHash hash, const index::Symbol& symbol) {
            if(symbol.name.empty())
                return;
            if(llvm::StringRef(symbol.name).lower().find(query_lower) == std::string::npos)
                return;
            auto def_loc = indexer.find_definition_location(hash);
            if(!def_loc)
                return;
            if(!seen.insert(hash).second)
                return;

            auto file = uri_to_path(def_loc->uri);
            int line_num = static_cast<int>(def_loc->range.start.line) + 1;

            if(loc.path.has_value() && !loc.path->empty()) {
                llvm::StringRef wanted(*loc.path);
                bool basename_only = wanted.find_last_of("/\\") == llvm::StringRef::npos;
                if(basename_only) {
                    if(llvm::sys::path::filename(file) != wanted)
                        return;
                } else if(!llvm::StringRef(file).ends_with(wanted)) {
                    return;
                }
            }

            bool is_exact = llvm::StringRef(symbol.name).lower() == query_lower ||
                            llvm::StringRef(symbol.name).ends_with("::" + *loc.name);

            ResolvedSymbol rs{hash, symbol.name, symbol.kind, std::move(file), line_num};
            if(is_exact)
                exact_matches.push_back(std::move(rs));
            else
                candidates.push_back(std::move(rs));
        };

        for(auto& [hash, symbol]: workspace.project_index.symbols)
            try_symbol(hash, symbol);
        indexer.foreach_session([&](std::uint32_t, const Session& session) -> bool {
            for(auto& [hash, symbol]: *session.symbols)
                try_symbol(hash, symbol);
            return true;
        });

        if(!exact_matches.empty())
            return exact_matches;
        return candidates;
    }

    if(loc.path.has_value() && loc.line.has_value()) {
        auto path_str = *loc.path;
        auto target_line = static_cast<protocol::uinteger>(*loc.line - 1);

        auto pool_it = workspace.path_pool.cache.find(path_str);
        auto server_id = pool_it != workspace.path_pool.cache.end() ? pool_it->second : ~0u;
        if(server_id != ~0u) {
            std::vector<ResolvedSymbol> session_result;
            indexer.with_session(server_id, [&](const Session& session) {
                auto map = session.line_map();
                for(auto& [hash, rels]: session.file_index->relations) {
                    for(auto& rel: rels) {
                        if(rel.kind.value() != RelationKind::Definition)
                            continue;
                        auto start = map.to_position(rel.range.begin);
                        if(start && start->line == target_line) {
                            std::string name;
                            SymbolKind kind;
                            if(!indexer.find_symbol_info(hash, name, kind))
                                continue;
                            if(kind == SymbolKind::Parameter || kind == SymbolKind::Label)
                                continue;
                            session_result.push_back(
                                {hash, std::move(name), kind, path_str, *loc.line});
                        }
                    }
                }
            });
            if(!session_result.empty())
                return session_result;
        }

        auto it = workspace.project_index.path_pool.find(path_str);
        if(it == workspace.project_index.path_pool.cache.end())
            return {};

        auto proj_id = it->second;
        auto shard_it = workspace.merged_indices.find(proj_id);
        if(shard_it == workspace.merged_indices.end())
            return {};

        auto& merged_index = shard_it->second;
        auto ls = merged_index.line_starts();
        if(ls.empty())
            return {};
        lsp::LineMap map(merged_index.content(), ls);

        for(auto& [hash, symbol]: workspace.project_index.symbols) {
            if(!symbol.reference_files.contains(proj_id))
                continue;
            bool found = false;
            merged_index.lookup(hash, RelationKind::Definition, [&](const index::Relation& r) {
                // FIXME: unchecked optional dereference
                auto range = map.to_range(r.range.begin, r.range.end);
                if(range && range->start.line == target_line) {
                    found = true;
                    return false;
                }
                return true;
            });
            if(found)
                return {
                    {hash, symbol.name, symbol.kind, path_str, *loc.line}
                };
        }

        return {};
    }

    return {};
}

/// Resolve a locator and require a unique match: no candidate is "symbol not
/// found", several candidates ask the client to disambiguate via symbolId.
static std::expected<ResolvedSymbol, kota::ipc::Error>
    resolve_unique(const agentic::ReadSymbolParams& loc, Workspace& workspace, Indexer& indexer) {
    auto candidates = resolve_locator(loc, workspace, indexer);
    if(candidates.empty())
        return std::unexpected(kota::ipc::Error{"symbol not found"});
    if(candidates.size() > 1) {
        return std::unexpected(
            kota::ipc::Error{std::format("ambiguous: {} candidates, use symbolId to disambiguate",
                                         candidates.size())});
    }
    return std::move(candidates[0]);
}

static std::uint64_t extract_symbol_id(const std::optional<protocol::LSPAny>& data) {
    if(!data.has_value())
        return 0;
    if(auto* val = std::get_if<std::int64_t>(&static_cast<const protocol::LSPVariant&>(*data)))
        return static_cast<std::uint64_t>(*val);
    LOG_WARN("extract_symbol_id: unexpected LSPAny variant type");
    return 0;
}

AgentClient::AgentClient(MasterServer& server, kota::ipc::JsonPeer& peer) :
    server(server), peer(peer) {
    using namespace agentic;

    auto& srv = this->server;

    peer.on_request(
        [&srv](RequestContext&,
               const CompileCommandParams& params) -> RequestResult<CompileCommandParams> {
            std::string directory;
            std::vector<std::string> arguments;
            srv.compiler.fill_compile_args(params.path, directory, arguments);

            co_return CompileCommandResult{
                .file = params.path,
                .directory = std::move(directory),
                .arguments = std::move(arguments),
            };
        });

    peer.on_request([&srv](RequestContext&,
                           const ProjectFilesParams& params) -> RequestResult<ProjectFilesParams> {
        auto& ws = srv.workspace;
        auto filter = params.filter.value_or("all");

        ProjectFilesResult result;
        llvm::DenseSet<std::uint32_t> seen;

        for(auto& entry: ws.cdb.get_entries()) {
            auto file_path = ws.cdb.resolve_path(entry.file);
            if(file_path.empty())
                continue;

            auto proj_it = ws.project_index.path_pool.find(file_path);
            if(proj_it != ws.project_index.path_pool.cache.end()) {
                if(!seen.insert(proj_it->second).second)
                    continue;
            }

            std::string kind_str;
            auto mod_it = ws.path_to_module.find(ws.path_pool.intern(file_path));
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
            for(auto& [path_id, shard]: ws.merged_indices) {
                if(seen.contains(path_id))
                    continue;
                auto path_str = ws.project_index.path_pool.path(path_id);
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
            auto& ws = srv.workspace;
            auto pool_it = ws.path_pool.cache.find(params.path);
            if(pool_it == ws.path_pool.cache.end())
                co_return FileDepsResult{.file = params.path};
            auto path_id = pool_it->second;
            auto direction = params.direction.value_or("both");
            auto max_depth = params.depth.value_or(1);

            FileDepsResult result;
            result.file = params.path;

            if(direction == "includes" || direction == "both") {
                auto includes = ws.dep_graph.get_all_includes(path_id);
                for(auto inc_id: includes) {
                    auto real_id = inc_id & DependencyGraph::PATH_ID_MASK;
                    auto inc_path = ws.path_pool.resolve(real_id);
                    result.includes.push_back(DepEntry{.path = inc_path.str(), .depth = 1});
                }

                if(max_depth == 0 || max_depth > 1) {
                    llvm::DenseSet<std::uint32_t> visited;
                    visited.insert(path_id);
                    for(auto& dep: result.includes)
                        visited.insert(ws.path_pool.intern(dep.path));

                    for(std::size_t i = 0; i < result.includes.size(); ++i) {
                        if(max_depth > 0 && result.includes[i].depth >= max_depth)
                            continue;
                        auto dep_id = ws.path_pool.intern(result.includes[i].path);
                        auto sub = ws.dep_graph.get_all_includes(dep_id);
                        for(auto sub_id: sub) {
                            auto real_id = sub_id & DependencyGraph::PATH_ID_MASK;
                            if(!visited.insert(real_id).second)
                                continue;
                            auto sub_path = ws.path_pool.resolve(real_id);
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
                    auto inc_path = ws.path_pool.resolve(inc_id);
                    result.includers.push_back(DepEntry{.path = inc_path.str(), .depth = 1});
                }

                if(max_depth == 0 || max_depth > 1) {
                    llvm::DenseSet<std::uint32_t> visited;
                    visited.insert(path_id);
                    for(auto& dep: result.includers) {
                        auto it = ws.path_pool.cache.find(dep.path);
                        if(it != ws.path_pool.cache.end())
                            visited.insert(it->second);
                    }

                    for(std::size_t i = 0; i < result.includers.size(); ++i) {
                        if(max_depth > 0 && result.includers[i].depth >= max_depth)
                            continue;
                        auto dep_it = ws.path_pool.cache.find(result.includers[i].path);
                        if(dep_it == ws.path_pool.cache.end())
                            continue;
                        auto sub = ws.dep_graph.get_includers(dep_it->second);
                        for(auto sub_id: sub) {
                            if(!visited.insert(sub_id).second)
                                continue;
                            auto sub_path = ws.path_pool.resolve(sub_id);
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
            auto& ws = srv.workspace;
            auto pool_it = ws.path_pool.cache.find(params.path);
            if(pool_it == ws.path_pool.cache.end())
                co_return ImpactAnalysisResult{};
            auto path_id = pool_it->second;

            ImpactAnalysisResult result;

            auto direct_includers = ws.dep_graph.get_includers(path_id);
            for(auto inc_id: direct_includers) {
                result.direct_dependents.push_back(ws.path_pool.resolve(inc_id).str());
            }

            auto hosts = ws.dep_graph.find_host_sources(path_id);
            llvm::DenseSet<std::uint32_t> seen;
            seen.insert(path_id);
            for(auto inc_id: direct_includers)
                seen.insert(inc_id);
            for(auto host_id: hosts) {
                if(seen.insert(host_id).second)
                    result.transitive_dependents.push_back(ws.path_pool.resolve(host_id).str());
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
        auto max = params.max_results.value_or(100);
        std::string query_lower = llvm::StringRef(params.query).lower();

        SymbolSearchResult result;
        llvm::DenseSet<index::SymbolHash> seen;

        auto try_symbol = [&](index::SymbolHash hash, const index::Symbol& symbol) {
            if(static_cast<int>(result.symbols.size()) >= max)
                return;
            if(symbol.name.empty())
                return;
            if(!query_lower.empty() &&
               llvm::StringRef(symbol.name).lower().find(query_lower) == std::string::npos)
                return;
            if(params.kind_filter.has_value()) {
                auto kind_name = std::string(symbol_kind_name(symbol.kind));
                auto& filter = *params.kind_filter;
                if(std::ranges::find(filter, kind_name) == filter.end())
                    return;
            }
            auto def_loc = srv.indexer.find_definition_location(hash);
            if(!def_loc)
                return;
            if(!seen.insert(hash).second)
                return;
            auto file = uri_to_path(def_loc->uri);
            result.symbols.push_back(SymbolEntry{
                .name = symbol.name,
                .kind = std::string(symbol_kind_name(symbol.kind)),
                .file = std::move(file),
                .line = static_cast<int>(def_loc->range.start.line) + 1,
                .symbol_id = hash,
            });
        };

        for(auto& [hash, symbol]: srv.workspace.project_index.symbols)
            try_symbol(hash, symbol);
        srv.indexer.foreach_session([&](std::uint32_t, const Session& session) -> bool {
            for(auto& [hash, symbol]: *session.symbols)
                try_symbol(hash, symbol);
            return true;
        });

        co_return result;
    });

    peer.on_request(
        [&srv](RequestContext&, const ReadSymbolParams& params) -> RequestResult<ReadSymbolParams> {
            auto resolved = resolve_unique(params, srv.workspace, srv.indexer);
            if(!resolved)
                co_return kota::outcome_error(std::move(resolved.error()));

            auto& rs = *resolved;
            auto def_text = srv.indexer.get_definition_text(rs.hash);
            if(!def_text)
                co_return kota::outcome_error(kota::ipc::Error{"definition not found"});

            co_return ReadSymbolResult{
                .name = rs.name,
                .kind = std::string(symbol_kind_name(rs.kind)),
                .file = std::move(def_text->file),
                .start_line = def_text->start_line,
                .end_line = def_text->end_line,
                .text = std::move(def_text->text),
                .symbol_id = rs.hash,
            };
        });

    peer.on_request(
        [&srv](RequestContext&,
               const DocumentSymbolsParams& params) -> RequestResult<DocumentSymbolsParams> {
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

            auto pool_it = srv.workspace.path_pool.cache.find(params.path);
            if(pool_it == srv.workspace.path_pool.cache.end())
                co_return result;
            auto server_id = pool_it->second;
            bool found_session = false;
            srv.indexer.with_session(server_id, [&](const Session& session) {
                found_session = true;
                for(auto& [hash, rels]: session.file_index->relations) {
                    for(auto& rel: rels) {
                        if(rel.kind.value() != RelationKind::Definition)
                            continue;
                        std::string name;
                        SymbolKind kind;
                        if(!srv.indexer.find_symbol_info(hash, name, kind))
                            continue;
                        if(!is_document_level(kind))
                            continue;
                        auto range = session.line_map().to_range(rel.range.begin, rel.range.end);
                        if(range) {
                            result.symbols.push_back(DocumentSymbolEntry{
                                .name = std::move(name),
                                .kind = std::string(symbol_kind_name(kind)),
                                .start_line = static_cast<int>(range->start.line) + 1,
                                .end_line = static_cast<int>(range->end.line) + 1,
                                .symbol_id = hash,
                            });
                            break;
                        }
                    }
                }
            });
            if(found_session)
                co_return result;

            auto it = srv.workspace.project_index.path_pool.find(params.path);
            if(it == srv.workspace.project_index.path_pool.cache.end())
                co_return result;

            auto proj_id = it->second;
            auto shard_it = srv.workspace.merged_indices.find(proj_id);
            if(shard_it == srv.workspace.merged_indices.end())
                co_return result;

            auto& merged_index = shard_it->second;
            auto ls = merged_index.line_starts();
            if(ls.empty())
                co_return result;
            lsp::LineMap map(merged_index.content(), ls);

            for(auto& [hash, symbol]: srv.workspace.project_index.symbols) {
                if(symbol.name.empty())
                    continue;
                if(!is_document_level(symbol.kind))
                    continue;
                if(!symbol.reference_files.contains(proj_id))
                    continue;

                merged_index.lookup(hash, RelationKind::Definition, [&](const index::Relation& r) {
                    // FIXME: unchecked optional dereference
                    auto range = map.to_range(r.range.begin, r.range.end);
                    if(range) {
                        result.symbols.push_back(DocumentSymbolEntry{
                            .name = symbol.name,
                            .kind = std::string(symbol_kind_name(symbol.kind)),
                            .start_line = static_cast<int>(range->start.line) + 1,
                            .end_line = static_cast<int>(range->end.line) + 1,
                            .symbol_id = hash,
                        });
                    }
                    return true;
                });
            }

            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&, const DefinitionParams& params) -> RequestResult<DefinitionParams> {
            auto resolved = resolve_unique(
                ReadSymbolParams{params.name, params.path, params.line, params.symbol_id},
                srv.workspace,
                srv.indexer);
            if(!resolved)
                co_return kota::outcome_error(std::move(resolved.error()));

            auto& rs = *resolved;

            DefinitionResult result;
            result.name = rs.name;
            result.kind = std::string(symbol_kind_name(rs.kind));
            result.symbol_id = rs.hash;

            if(auto def_text = srv.indexer.get_definition_text(rs.hash)) {
                result.definition = LocationEntry{
                    .file = std::move(def_text->file),
                    .start_line = def_text->start_line,
                    .end_line = def_text->end_line,
                    .text = std::move(def_text->text),
                };
            }

            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&, const ReferencesParams& params) -> RequestResult<ReferencesParams> {
            auto resolved = resolve_unique(
                ReadSymbolParams{params.name, params.path, params.line, params.symbol_id},
                srv.workspace,
                srv.indexer);
            if(!resolved)
                co_return kota::outcome_error(std::move(resolved.error()));

            auto& rs = *resolved;

            ReferencesResult result;
            result.name = rs.name;
            result.kind = std::string(symbol_kind_name(rs.kind));
            result.symbol_id = rs.hash;

            for(auto& ref: srv.indexer.collect_references(rs.hash, RelationKind::Reference)) {
                result.references.push_back(ReferenceEntry{
                    .file = std::move(ref.file),
                    .line = ref.line,
                    .context = std::move(ref.context),
                });
            }
            if(params.include_declaration.value_or(false)) {
                for(auto& ref: srv.indexer.collect_references(rs.hash, RelationKind::Definition)) {
                    result.references.push_back(ReferenceEntry{
                        .file = std::move(ref.file),
                        .line = ref.line,
                        .context = std::move(ref.context),
                    });
                }
            }

            result.total = static_cast<int>(result.references.size());
            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&, const CallGraphParams& params) -> RequestResult<CallGraphParams> {
            auto resolved = resolve_unique(
                ReadSymbolParams{params.name, params.path, params.line, params.symbol_id},
                srv.workspace,
                srv.indexer);
            if(!resolved)
                co_return kota::outcome_error(std::move(resolved.error()));

            auto& rs = *resolved;
            auto direction = params.direction.value_or("both");

            CallGraphResult result;
            result.root = CallGraphEntry{
                .name = rs.name,
                .kind = std::string(symbol_kind_name(rs.kind)),
                .file = rs.file,
                .line = rs.line,
                .symbol_id = rs.hash,
            };

            auto resolve_kind = [&](std::uint64_t sym_id) -> std::string {
                if(sym_id == 0)
                    return "Function";
                std::string name;
                SymbolKind kind;
                if(srv.indexer.find_symbol_info(sym_id, name, kind))
                    return std::string(symbol_kind_name(kind));
                return "Function";
            };

            if(direction == "callers" || direction == "both") {
                auto incoming = srv.indexer.find_incoming_calls(rs.hash);
                for(auto& call: incoming) {
                    auto sid = extract_symbol_id(call.from.data);
                    result.callers.push_back(CallGraphEntry{
                        .name = call.from.name,
                        .kind = resolve_kind(sid),
                        .file = uri_to_path(call.from.uri),
                        .line = static_cast<int>(call.from.range.start.line) + 1,
                        .symbol_id = sid,
                    });
                }
            }

            if(direction == "callees" || direction == "both") {
                auto outgoing = srv.indexer.find_outgoing_calls(rs.hash);
                for(auto& call: outgoing) {
                    auto sid = extract_symbol_id(call.to.data);
                    result.callees.push_back(CallGraphEntry{
                        .name = call.to.name,
                        .kind = resolve_kind(sid),
                        .file = uri_to_path(call.to.uri),
                        .line = static_cast<int>(call.to.range.start.line) + 1,
                        .symbol_id = sid,
                    });
                }
            }

            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&,
               const TypeHierarchyParams& params) -> RequestResult<TypeHierarchyParams> {
            auto resolved = resolve_unique(
                ReadSymbolParams{params.name, params.path, params.line, params.symbol_id},
                srv.workspace,
                srv.indexer);
            if(!resolved)
                co_return kota::outcome_error(std::move(resolved.error()));

            auto& rs = *resolved;
            auto direction = params.direction.value_or("both");

            TypeHierarchyResult result;
            result.root = TypeHierarchyEntry{
                .name = rs.name,
                .kind = std::string(symbol_kind_name(rs.kind)),
                .file = rs.file,
                .line = rs.line,
                .symbol_id = rs.hash,
            };

            auto resolve_kind = [&](std::uint64_t sym_id) -> std::string {
                if(sym_id == 0)
                    return "Class";
                std::string name;
                SymbolKind kind;
                if(srv.indexer.find_symbol_info(sym_id, name, kind))
                    return std::string(symbol_kind_name(kind));
                return "Class";
            };

            if(direction == "supertypes" || direction == "both") {
                for(auto& item: srv.indexer.find_supertypes(rs.hash)) {
                    auto sid = extract_symbol_id(item.data);
                    result.supertypes.push_back(TypeHierarchyEntry{
                        .name = item.name,
                        .kind = resolve_kind(sid),
                        .file = uri_to_path(item.uri),
                        .line = static_cast<int>(item.range.start.line) + 1,
                        .symbol_id = sid,
                    });
                }
            }

            if(direction == "subtypes" || direction == "both") {
                for(auto& item: srv.indexer.find_subtypes(rs.hash)) {
                    auto sid = extract_symbol_id(item.data);
                    result.subtypes.push_back(TypeHierarchyEntry{
                        .name = item.name,
                        .kind = resolve_kind(sid),
                        .file = uri_to_path(item.uri),
                        .line = static_cast<int>(item.range.start.line) + 1,
                        .symbol_id = sid,
                    });
                }
            }

            co_return result;
        });

    peer.on_request([&srv](RequestContext&, const StatusParams&) -> RequestResult<StatusParams> {
        StatusResult result;
        result.idle = srv.indexer.is_idle();
        result.pending = static_cast<int>(srv.indexer.pending_files());
        result.total = static_cast<int>(srv.indexer.total_queued());
        result.indexed = std::max(0, result.total - result.pending);
        co_return result;
    });

    peer.on_notification([&srv](const ShutdownParams&) {
        LOG_INFO("agentic/shutdown received, shutting down");
        srv.schedule_shutdown();
    });
}

}  // namespace clice
