#include "server/service/agent_client.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <vector>

#include "server/protocol/agentic.h"
#include "server/service/master_server.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/ipc/lsp/uri.h"
#include "kota/meta/enum.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MemoryBuffer.h"

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

static std::string read_file_line(llvm::StringRef file_path, int line) {
    auto buf = llvm::MemoryBuffer::getFile(file_path);
    if(!buf)
        return {};
    llvm::StringRef content = (*buf)->getBuffer();
    int current = 1;
    std::size_t pos = 0;
    while(current < line && pos < content.size()) {
        if(content[pos] == '\n')
            ++current;
        ++pos;
    }
    if(current != line)
        return {};
    auto end = content.find('\n', pos);
    if(end == llvm::StringRef::npos)
        end = content.size();
    return content.slice(pos, end).str();
}

static std::string read_file_lines(llvm::StringRef file_path, int start_line, int end_line) {
    auto buf = llvm::MemoryBuffer::getFile(file_path);
    if(!buf)
        return {};
    llvm::StringRef content = (*buf)->getBuffer();
    int current = 1;
    std::size_t start_pos = 0;
    while(current < start_line && start_pos < content.size()) {
        if(content[start_pos] == '\n')
            ++current;
        ++start_pos;
    }
    if(current != start_line)
        return {};
    std::size_t end_pos = start_pos;
    while(current <= end_line && end_pos < content.size()) {
        if(content[end_pos] == '\n')
            ++current;
        ++end_pos;
    }
    return content.slice(start_pos, end_pos).str();
}

struct DefinitionText {
    std::string text;
    int end_line;
};

static DefinitionText read_definition_text(llvm::StringRef file_path, int start_line) {
    auto buf = llvm::MemoryBuffer::getFile(file_path);
    if(!buf)
        return {"", start_line};
    llvm::StringRef content = (*buf)->getBuffer();

    int current = 1;
    std::size_t start_pos = 0;
    while(current < start_line && start_pos < content.size()) {
        if(content[start_pos] == '\n')
            ++current;
        ++start_pos;
    }
    if(current != start_line)
        return {"", start_line};

    int depth = 0;
    bool found_brace = false;
    std::size_t end_pos = start_pos;
    int end_line = start_line;

    while(end_pos < content.size()) {
        char c = content[end_pos];
        if(c == '{') {
            ++depth;
            found_brace = true;
        } else if(c == '}') {
            --depth;
            if(found_brace && depth == 0) {
                ++end_pos;
                while(end_pos < content.size() && content[end_pos] != '\n')
                    ++end_pos;
                if(end_pos < content.size())
                    ++end_pos;
                return {content.slice(start_pos, end_pos).str(), end_line};
            }
        } else if(c == '\n') {
            ++end_line;
        } else if(c == '/' && end_pos + 1 < content.size()) {
            if(content[end_pos + 1] == '/') {
                while(end_pos < content.size() && content[end_pos] != '\n')
                    ++end_pos;
                continue;
            }
        } else if(c == '\'' || c == '"') {
            char quote = c;
            ++end_pos;
            while(end_pos < content.size() && content[end_pos] != quote) {
                if(content[end_pos] == '\\')
                    ++end_pos;
                ++end_pos;
            }
        }

        if(!found_brace && c == ';') {
            ++end_pos;
            while(end_pos < content.size() && content[end_pos] != '\n')
                ++end_pos;
            if(end_pos < content.size())
                ++end_pos;
            return {content.slice(start_pos, end_pos).str(), end_line};
        }

        ++end_pos;
    }

    return {content.slice(start_pos, end_pos).str(), end_line};
}

static std::string path_from_uri(llvm::StringRef uri) {
    auto parsed = lsp::URI::parse(uri);
    if(parsed.has_value()) {
        auto fp = parsed->file_path();
        if(fp.has_value())
            return std::move(*fp);
    }
    return uri.str();
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
                                                   llvm::DenseMap<std::uint32_t, Session>& sessions,
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
        auto file = path_from_uri(def_loc->uri);
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

            auto file = path_from_uri(def_loc->uri);
            int line_num = static_cast<int>(def_loc->range.start.line) + 1;

            if(loc.path.has_value() && !loc.path->empty()) {
                if(!llvm::StringRef(file).ends_with(*loc.path) &&
                   !llvm::StringRef(*loc.path).ends_with(llvm::sys::path::filename(file)))
                    return;
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
        for(auto& [_, sess]: sessions) {
            if(!sess.file_index)
                continue;
            for(auto& [hash, symbol]: sess.file_index->symbols)
                try_symbol(hash, symbol);
        }

        if(!exact_matches.empty())
            return exact_matches;
        return candidates;
    }

    if(loc.path.has_value() && loc.line.has_value()) {
        auto path_str = *loc.path;
        auto target_line = static_cast<protocol::uinteger>(*loc.line - 1);

        auto server_id = workspace.path_pool.intern(path_str);
        auto* sess = sessions.contains(server_id) ? &sessions[server_id] : nullptr;
        if(sess && sess->file_index) {
            auto& fi = *sess->file_index;
            if(fi.mapper) {
                for(auto& [hash, rels]: fi.file_index.relations) {
                    for(auto& rel: rels) {
                        if(rel.kind.value() != RelationKind::Definition)
                            continue;
                        auto start = fi.mapper->to_position(rel.range.begin);
                        if(start && start->line == target_line) {
                            std::string name;
                            SymbolKind kind;
                            if(indexer.find_symbol_info(hash, name, kind))
                                return {
                                    {hash, std::move(name), kind, path_str, *loc.line}
                                };
                        }
                    }
                }
            }
        }

        auto it = workspace.project_index.path_pool.find(path_str);
        if(it == workspace.project_index.path_pool.cache.end())
            return {};

        auto proj_id = it->second;
        auto shard_it = workspace.merged_indices.find(proj_id);
        if(shard_it == workspace.merged_indices.end())
            return {};

        for(auto& [hash, symbol]: workspace.project_index.symbols) {
            if(!symbol.reference_files.contains(proj_id))
                continue;
            bool found = false;
            shard_it->second.find_relations(hash,
                                            RelationKind::Definition,
                                            [&](const index::Relation&, protocol::Range range) {
                                                if(range.start.line == target_line) {
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

static agentic::SymbolEntry to_symbol_entry(const ResolvedSymbol& rs) {
    return {
        .name = rs.name,
        .kind = std::string(symbol_kind_name(rs.kind)),
        .file = rs.file,
        .line = rs.line,
        .symbol_id = rs.hash,
    };
}

static std::uint64_t extract_symbol_id(const std::optional<protocol::LSPAny>& data) {
    if(!data.has_value())
        return 0;
    if(auto* val = std::get_if<std::int64_t>(&static_cast<const protocol::LSPVariant&>(*data)))
        return static_cast<std::uint64_t>(*val);
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
            if(!srv.compiler.fill_compile_args(params.path, directory, arguments)) {
                co_return kota::outcome_error(
                    kota::ipc::Error{std::format("no compile command found for {}", params.path)});
            }

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
            if(!seen.insert(entry.file).second)
                continue;
            auto file_path = ws.cdb.resolve_path(entry.file);
            if(file_path.empty())
                continue;

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
            auto path_id = ws.path_pool.intern(params.path);
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
            }

            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&,
               const ImpactAnalysisParams& params) -> RequestResult<ImpactAnalysisParams> {
            auto& ws = srv.workspace;
            auto path_id = ws.path_pool.intern(params.path);

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
            auto file = path_from_uri(def_loc->uri);
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
        for(auto& [_, sess]: srv.sessions) {
            if(!sess.file_index)
                continue;
            for(auto& [hash, symbol]: sess.file_index->symbols)
                try_symbol(hash, symbol);
        }

        co_return result;
    });

    peer.on_request(
        [&srv](RequestContext&, const ReadSymbolParams& params) -> RequestResult<ReadSymbolParams> {
            auto candidates = resolve_locator(params, srv.workspace, srv.sessions, srv.indexer);
            if(candidates.empty())
                co_return kota::outcome_error(kota::ipc::Error{"symbol not found"});
            if(candidates.size() > 1) {
                co_return kota::outcome_error(kota::ipc::Error{
                    std::format("ambiguous: {} candidates, use symbolId to disambiguate",
                                candidates.size())});
            }

            auto& rs = candidates[0];
            auto def_loc = srv.indexer.find_definition_location(rs.hash);
            if(!def_loc)
                co_return kota::outcome_error(kota::ipc::Error{"definition not found"});

            auto file = path_from_uri(def_loc->uri);
            int start = static_cast<int>(def_loc->range.start.line) + 1;
            auto [text, end] = read_definition_text(file, start);

            co_return ReadSymbolResult{
                .name = rs.name,
                .kind = std::string(symbol_kind_name(rs.kind)),
                .file = std::move(file),
                .start_line = start,
                .end_line = end,
                .text = std::move(text),
                .symbol_id = rs.hash,
            };
        });

    peer.on_request(
        [&srv](RequestContext&,
               const DocumentSymbolsParams& params) -> RequestResult<DocumentSymbolsParams> {
            auto is_document_level = [](SymbolKind kind) {
                return kind != SymbolKind::Parameter && kind != SymbolKind::Label &&
                       kind != SymbolKind::MacroParameter;
            };

            DocumentSymbolsResult result;

            auto server_id = srv.workspace.path_pool.intern(params.path);
            auto sess_it = srv.sessions.find(server_id);
            if(sess_it != srv.sessions.end() && sess_it->second.file_index) {
                auto& fi = *sess_it->second.file_index;
                for(auto& [hash, rels]: fi.file_index.relations) {
                    for(auto& rel: rels) {
                        if(rel.kind.value() != RelationKind::Definition)
                            continue;
                        std::string name;
                        SymbolKind kind;
                        if(!srv.indexer.find_symbol_info(hash, name, kind))
                            continue;
                        if(!is_document_level(kind))
                            continue;
                        if(fi.mapper) {
                            auto start = fi.mapper->to_position(rel.range.begin);
                            auto end = fi.mapper->to_position(rel.range.end);
                            if(start && end) {
                                result.symbols.push_back(DocumentSymbolEntry{
                                    .name = std::move(name),
                                    .kind = std::string(symbol_kind_name(kind)),
                                    .start_line = static_cast<int>(start->line) + 1,
                                    .end_line = static_cast<int>(end->line) + 1,
                                    .symbol_id = hash,
                                });
                            }
                        }
                        break;
                    }
                }
                co_return result;
            }

            auto it = srv.workspace.project_index.path_pool.find(params.path);
            if(it == srv.workspace.project_index.path_pool.cache.end())
                co_return result;

            auto proj_id = it->second;
            auto shard_it = srv.workspace.merged_indices.find(proj_id);
            if(shard_it == srv.workspace.merged_indices.end())
                co_return result;

            for(auto& [hash, symbol]: srv.workspace.project_index.symbols) {
                if(symbol.name.empty())
                    continue;
                if(!is_document_level(symbol.kind))
                    continue;
                if(!symbol.reference_files.contains(proj_id))
                    continue;

                shard_it->second.find_relations(
                    hash,
                    RelationKind::Definition,
                    [&](const index::Relation&, protocol::Range range) {
                        result.symbols.push_back(DocumentSymbolEntry{
                            .name = symbol.name,
                            .kind = std::string(symbol_kind_name(symbol.kind)),
                            .start_line = static_cast<int>(range.start.line) + 1,
                            .end_line = static_cast<int>(range.end.line) + 1,
                            .symbol_id = hash,
                        });
                        return true;
                    });
            }

            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&, const DefinitionParams& params) -> RequestResult<DefinitionParams> {
            auto candidates = resolve_locator(
                ReadSymbolParams{params.name, params.path, params.line, params.symbol_id},
                srv.workspace,
                srv.sessions,
                srv.indexer);
            if(candidates.empty())
                co_return kota::outcome_error(kota::ipc::Error{"symbol not found"});
            if(candidates.size() > 1) {
                co_return kota::outcome_error(kota::ipc::Error{
                    std::format("ambiguous: {} candidates, use symbolId to disambiguate",
                                candidates.size())});
            }

            auto& rs = candidates[0];
            auto def_loc = srv.indexer.find_definition_location(rs.hash);

            DefinitionResult result;
            result.name = rs.name;
            result.kind = std::string(symbol_kind_name(rs.kind));
            result.symbol_id = rs.hash;

            if(def_loc) {
                auto file = path_from_uri(def_loc->uri);
                int start = static_cast<int>(def_loc->range.start.line) + 1;
                auto [text, end] = read_definition_text(file, start);
                result.definition = LocationEntry{
                    .file = std::move(file),
                    .start_line = start,
                    .end_line = end,
                    .text = std::move(text),
                };
            }

            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&, const ReferencesParams& params) -> RequestResult<ReferencesParams> {
            auto candidates = resolve_locator(
                ReadSymbolParams{params.name, params.path, params.line, params.symbol_id},
                srv.workspace,
                srv.sessions,
                srv.indexer);
            if(candidates.empty())
                co_return kota::outcome_error(kota::ipc::Error{"symbol not found"});
            if(candidates.size() > 1) {
                co_return kota::outcome_error(kota::ipc::Error{
                    std::format("ambiguous: {} candidates, use symbolId to disambiguate",
                                candidates.size())});
            }

            auto& rs = candidates[0];
            auto& ws = srv.workspace;

            ReferencesResult result;
            result.name = rs.name;
            result.kind = std::string(symbol_kind_name(rs.kind));
            result.symbol_id = rs.hash;

            auto collect = [&](RelationKind kind) {
                auto sym_it = ws.project_index.symbols.find(rs.hash);
                if(sym_it != ws.project_index.symbols.end()) {
                    for(auto file_id: sym_it->second.reference_files) {
                        auto shard_it = ws.merged_indices.find(file_id);
                        if(shard_it == ws.merged_indices.end())
                            continue;
                        auto file_path = ws.project_index.path_pool.path(file_id);
                        shard_it->second.find_relations(
                            rs.hash,
                            kind,
                            [&](const auto&, protocol::Range range) {
                                int line_num = static_cast<int>(range.start.line) + 1;
                                result.references.push_back(ReferenceEntry{
                                    .file = file_path.str(),
                                    .line = line_num,
                                    .context = read_file_line(file_path, line_num),
                                });
                                return true;
                            });
                    }
                }
                for(auto& [sid, sess]: srv.sessions) {
                    if(!sess.file_index)
                        continue;
                    auto file_path = ws.path_pool.resolve(sid);
                    sess.file_index->find_relations(
                        rs.hash,
                        kind,
                        [&](const auto&, protocol::Range range) {
                            int line_num = static_cast<int>(range.start.line) + 1;
                            result.references.push_back(ReferenceEntry{
                                .file = file_path.str(),
                                .line = line_num,
                                .context = read_file_line(file_path, line_num),
                            });
                            return true;
                        });
                }
            };

            collect(RelationKind::Reference);
            if(params.include_declaration.value_or(false))
                collect(RelationKind::Definition);

            result.total = static_cast<int>(result.references.size());
            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&, const CallGraphParams& params) -> RequestResult<CallGraphParams> {
            auto candidates = resolve_locator(
                ReadSymbolParams{params.name, params.path, params.line, params.symbol_id},
                srv.workspace,
                srv.sessions,
                srv.indexer);
            if(candidates.empty())
                co_return kota::outcome_error(kota::ipc::Error{"symbol not found"});
            if(candidates.size() > 1) {
                co_return kota::outcome_error(kota::ipc::Error{
                    std::format("ambiguous: {} candidates, use symbolId to disambiguate",
                                candidates.size())});
            }

            auto& rs = candidates[0];
            auto direction = params.direction.value_or("both");

            CallGraphResult result;
            result.root = CallGraphEntry{
                .name = rs.name,
                .kind = std::string(symbol_kind_name(rs.kind)),
                .file = rs.file,
                .line = rs.line,
                .symbol_id = rs.hash,
            };

            if(direction == "callers" || direction == "both") {
                auto incoming = srv.indexer.find_incoming_calls(rs.hash);
                for(auto& call: incoming) {
                    result.callers.push_back(CallGraphEntry{
                        .name = call.from.name,
                        .kind = std::string("Function"),
                        .file = path_from_uri(call.from.uri),
                        .line = static_cast<int>(call.from.range.start.line) + 1,
                        .symbol_id = extract_symbol_id(call.from.data),
                    });
                }
            }

            if(direction == "callees" || direction == "both") {
                auto outgoing = srv.indexer.find_outgoing_calls(rs.hash);
                for(auto& call: outgoing) {
                    result.callees.push_back(CallGraphEntry{
                        .name = call.to.name,
                        .kind = std::string("Function"),
                        .file = path_from_uri(call.to.uri),
                        .line = static_cast<int>(call.to.range.start.line) + 1,
                        .symbol_id = extract_symbol_id(call.to.data),
                    });
                }
            }

            co_return result;
        });

    peer.on_request(
        [&srv](RequestContext&,
               const TypeHierarchyParams& params) -> RequestResult<TypeHierarchyParams> {
            auto candidates = resolve_locator(
                ReadSymbolParams{params.name, params.path, params.line, params.symbol_id},
                srv.workspace,
                srv.sessions,
                srv.indexer);
            if(candidates.empty())
                co_return kota::outcome_error(kota::ipc::Error{"symbol not found"});
            if(candidates.size() > 1) {
                co_return kota::outcome_error(kota::ipc::Error{
                    std::format("ambiguous: {} candidates, use symbolId to disambiguate",
                                candidates.size())});
            }

            auto& rs = candidates[0];
            auto direction = params.direction.value_or("both");

            TypeHierarchyResult result;
            result.root = TypeHierarchyEntry{
                .name = rs.name,
                .kind = std::string(symbol_kind_name(rs.kind)),
                .file = rs.file,
                .line = rs.line,
                .symbol_id = rs.hash,
            };

            if(direction == "supertypes" || direction == "both") {
                for(auto& item: srv.indexer.find_supertypes(rs.hash)) {
                    result.supertypes.push_back(TypeHierarchyEntry{
                        .name = item.name,
                        .kind = std::string("Class"),
                        .file = path_from_uri(item.uri),
                        .line = static_cast<int>(item.range.start.line) + 1,
                        .symbol_id = extract_symbol_id(item.data),
                    });
                }
            }

            if(direction == "subtypes" || direction == "both") {
                for(auto& item: srv.indexer.find_subtypes(rs.hash)) {
                    result.subtypes.push_back(TypeHierarchyEntry{
                        .name = item.name,
                        .kind = std::string("Class"),
                        .file = path_from_uri(item.uri),
                        .line = static_cast<int>(item.range.start.line) + 1,
                        .symbol_id = extract_symbol_id(item.data),
                    });
                }
            }

            co_return result;
        });

    peer.on_request([&srv](RequestContext&, const StatusParams&) -> RequestResult<StatusParams> {
        auto& ws = srv.workspace;
        StatusResult result;
        result.idle = srv.indexer.is_idle();
        result.pending = static_cast<int>(srv.indexer.pending_files());
        result.total = static_cast<int>(ws.cdb.get_entries().size());
        result.indexed = static_cast<int>(ws.merged_indices.size());
        co_return result;
    });
}

}  // namespace clice
