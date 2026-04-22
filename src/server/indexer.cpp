#include "server/indexer.h"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

#include "index/tu_index.h"
#include "server/compiler.h"
#include "server/protocol.h"
#include "server/session.h"
#include "server/worker_pool.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/uri.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace clice {

namespace lsp = kota::ipc::lsp;

void Indexer::merge(const void* tu_index_data, std::size_t size) {
    auto tu_index = index::TUIndex::from(tu_index_data);
    if(tu_index.graph.paths.empty()) {
        LOG_WARN("Ignoring TUIndex with empty path graph");
        return;
    }
    auto file_ids_map = workspace.project_index.merge(tu_index);
    auto main_tu_path_id = static_cast<std::uint32_t>(tu_index.graph.paths.size() - 1);

    auto merge_file_index = [&](std::uint32_t tu_path_id, index::FileIndex& file_idx) {
        auto global_path_id = file_ids_map[tu_path_id];
        auto& shard = workspace.merged_indices[global_path_id];

        if(tu_path_id == main_tu_path_id) {
            std::vector<index::IncludeLocation> include_locs;
            for(auto& loc: tu_index.graph.locations) {
                index::IncludeLocation remapped = loc;
                remapped.path_id = file_ids_map[loc.path_id];
                include_locs.push_back(remapped);
            }
            auto file_path = workspace.project_index.path_pool.path(global_path_id);
            llvm::StringRef file_content;
            std::string file_content_storage;
            auto buf = llvm::MemoryBuffer::getFile(file_path);
            if(buf) {
                file_content_storage = (*buf)->getBuffer().str();
                file_content = file_content_storage;
            }
            shard.index.merge(global_path_id,
                              tu_index.built_at,
                              std::move(include_locs),
                              file_idx,
                              file_content);
        } else {
            std::optional<std::uint32_t> include_id;
            for(std::uint32_t i = 0; i < tu_index.graph.locations.size(); ++i) {
                if(tu_index.graph.locations[i].path_id == tu_path_id) {
                    include_id = i;
                    break;
                }
            }
            if(!include_id) {
                LOG_WARN("Skip merge for path {}: include location not found", global_path_id);
                return;
            }
            auto header_path = workspace.project_index.path_pool.path(global_path_id);
            llvm::StringRef header_content;
            std::string header_content_storage;
            auto header_buf = llvm::MemoryBuffer::getFile(header_path);
            if(header_buf) {
                header_content_storage = (*header_buf)->getBuffer().str();
                header_content = header_content_storage;
            }
            shard.index.merge(global_path_id, *include_id, file_idx, header_content);
        }
        shard.invalidate_mapper();
    };

    for(auto& [tu_path_id, file_idx]: tu_index.path_file_indices) {
        merge_file_index(tu_path_id, file_idx);
    }
    merge_file_index(main_tu_path_id, tu_index.main_file_index);

    LOG_INFO("Merged TUIndex: {} paths, {} symbols, {} merged_shards",
             tu_index.graph.paths.size(),
             tu_index.symbols.size(),
             workspace.merged_indices.size());
}

void Indexer::save(llvm::StringRef index_dir) {
    if(index_dir.empty())
        return;

    auto ec = llvm::sys::fs::create_directories(index_dir);
    if(ec) {
        LOG_WARN("Failed to create index directory {}: {}", std::string(index_dir), ec.message());
        return;
    }

    auto project_path = path::join(index_dir, "project.idx");
    {
        std::error_code write_ec;
        llvm::raw_fd_ostream os(project_path, write_ec);
        if(!write_ec) {
            workspace.project_index.serialize(os);
            LOG_INFO("Saved ProjectIndex to {}", project_path);
        } else {
            LOG_WARN("Failed to save ProjectIndex: {}", write_ec.message());
        }
    }

    auto shards_dir = path::join(index_dir, "shards");
    ec = llvm::sys::fs::create_directories(shards_dir);
    if(ec) {
        LOG_WARN("Failed to create shards directory: {}", ec.message());
        return;
    }

    std::size_t saved = 0;
    for(auto& [path_id, shard]: workspace.merged_indices) {
        if(!shard.index.need_rewrite())
            continue;
        auto shard_path = path::join(shards_dir, std::to_string(path_id) + ".idx");
        std::error_code write_ec;
        llvm::raw_fd_ostream os(shard_path, write_ec);
        if(!write_ec) {
            shard.index.serialize(os);
            ++saved;
        }
    }
    LOG_INFO("Saved {} MergedIndex shards (of {} total)", saved, workspace.merged_indices.size());
}

void Indexer::load(llvm::StringRef index_dir) {
    if(index_dir.empty())
        return;

    auto project_path = path::join(index_dir, "project.idx");
    auto buf = llvm::MemoryBuffer::getFile(project_path);
    if(buf) {
        workspace.project_index = index::ProjectIndex::from((*buf)->getBufferStart());
        LOG_INFO("Loaded ProjectIndex: {} symbols", workspace.project_index.symbols.size());
    }

    auto shards_dir = path::join(index_dir, "shards");
    std::error_code ec;
    for(auto it = llvm::sys::fs::directory_iterator(shards_dir, ec);
        !ec && it != llvm::sys::fs::directory_iterator();
        it.increment(ec)) {
        auto filename = llvm::sys::path::filename(it->path());
        if(!filename.ends_with(".idx"))
            continue;
        auto stem = filename.drop_back(4);
        std::uint32_t path_id = 0;
        if(stem.getAsInteger(10, path_id))
            continue;
        workspace.merged_indices[path_id] = MergedIndexShard{index::MergedIndex::load(it->path())};
    }

    if(!workspace.merged_indices.empty()) {
        LOG_INFO("Loaded {} MergedIndex shards", workspace.merged_indices.size());
    }
}

bool Indexer::need_update(llvm::StringRef file_path) {
    auto cache_it = workspace.project_index.path_pool.find(file_path);
    if(cache_it == workspace.project_index.path_pool.cache.end())
        return true;

    auto merged_it = workspace.merged_indices.find(cache_it->second);
    if(merged_it == workspace.merged_indices.end())
        return true;

    llvm::SmallVector<llvm::StringRef> path_mapping;
    for(auto& p: workspace.project_index.path_pool.paths) {
        path_mapping.push_back(p);
    }
    return merged_it->second.index.need_update(path_mapping);
}

bool Indexer::find_symbol_info(index::SymbolHash hash, std::string& name, SymbolKind& kind) const {
    for(auto& [_, session]: sessions) {
        if(!session.file_index)
            continue;
        auto it = session.file_index->symbols.find(hash);
        if(it != session.file_index->symbols.end()) {
            name = it->second.name;
            kind = it->second.kind;
            return true;
        }
    }
    auto it = workspace.project_index.symbols.find(hash);
    if(it != workspace.project_index.symbols.end()) {
        name = it->second.name;
        kind = it->second.kind;
        return true;
    }
    return false;
}

Indexer::CursorHit Indexer::resolve_cursor(llvm::StringRef path,
                                           const protocol::Position& position,
                                           Session* session) {
    // Try the session's open file index first.
    if(session && session->file_index) {
        auto& index = *session->file_index;
        if(!index.mapper)
            return {};
        auto offset = index.mapper->to_offset(position);
        if(!offset)
            return {};
        if(auto found = index.find_occurrence(*offset))
            return {found->first, found->second};
        return {};
    }

    // Fallback to MergedIndex, using session text (or reading from disk) for position -> offset.
    const std::string* doc_text = session ? &session->text : nullptr;
    if(!doc_text)
        return {};
    lsp::PositionMapper doc_mapper(*doc_text, lsp::PositionEncoding::UTF16);
    auto offset = doc_mapper.to_offset(position);
    if(!offset)
        return {};

    auto proj_it = workspace.project_index.path_pool.find(path);
    if(proj_it == workspace.project_index.path_pool.cache.end())
        return {};
    auto shard_it = workspace.merged_indices.find(proj_it->second);
    if(shard_it == workspace.merged_indices.end())
        return {};

    if(auto found = shard_it->second.find_occurrence(*offset))
        return {found->first, found->second};
    return {};
}

std::vector<protocol::Location> Indexer::query_relations(llvm::StringRef path,
                                                         const protocol::Position& position,
                                                         RelationKind kind,
                                                         Session* session) {
    auto hit = resolve_cursor(path, position, session);
    if(hit.hash == 0)
        return {};

    std::vector<protocol::Location> locations;

    auto sym_it = workspace.project_index.symbols.find(hit.hash);
    if(sym_it != workspace.project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(is_proj_path_open(file_id))
                continue;
            auto shard_it = workspace.merged_indices.find(file_id);
            if(shard_it == workspace.merged_indices.end())
                continue;
            auto uri = lsp::URI::from_file_path(workspace.project_index.path_pool.path(file_id));
            if(!uri)
                continue;
            shard_it->second.find_relations(hit.hash,
                                            kind,
                                            [&](const auto&, protocol::Range range) {
                                                locations.push_back({uri->str(), range});
                                                return true;
                                            });
        }
    }

    for(auto& [id, sess]: sessions) {
        if(!sess.file_index)
            continue;
        auto uri = lsp::URI::from_file_path(std::string(workspace.path_pool.resolve(id)));
        if(!uri)
            continue;
        sess.file_index->find_relations(hit.hash, kind, [&](const auto&, protocol::Range range) {
            locations.push_back({uri->str(), range});
            return true;
        });
    }

    return locations;
}

std::optional<SymbolInfo> Indexer::lookup_symbol(const std::string& uri,
                                                 llvm::StringRef path,
                                                 const protocol::Position& position,
                                                 Session* session) {
    auto hit = resolve_cursor(path, position, session);
    if(hit.hash == 0)
        return std::nullopt;

    std::string name;
    SymbolKind sym_kind;
    if(!find_symbol_info(hit.hash, name, sym_kind))
        return std::nullopt;

    return SymbolInfo{hit.hash, std::move(name), sym_kind, uri, hit.range};
}

std::optional<protocol::Location> Indexer::find_definition_location(index::SymbolHash hash) {
    // Open file indices first (fresher data for actively-edited files).
    for(auto& [id, sess]: sessions) {
        if(!sess.file_index)
            continue;
        auto uri = lsp::URI::from_file_path(std::string(workspace.path_pool.resolve(id)));
        if(!uri)
            continue;
        std::optional<protocol::Location> result;
        sess.file_index->find_relations(hash,
                                        RelationKind::Definition,
                                        [&](const auto&, protocol::Range range) {
                                            result = protocol::Location{uri->str(), range};
                                            return false;
                                        });
        if(result)
            return result;
    }

    // Fall back to ProjectIndex reference files.
    auto sym_it = workspace.project_index.symbols.find(hash);
    if(sym_it == workspace.project_index.symbols.end())
        return std::nullopt;

    for(auto file_id: sym_it->second.reference_files) {
        if(is_proj_path_open(file_id))
            continue;
        auto shard_it = workspace.merged_indices.find(file_id);
        if(shard_it == workspace.merged_indices.end())
            continue;
        auto uri = lsp::URI::from_file_path(workspace.project_index.path_pool.path(file_id));
        if(!uri)
            continue;
        std::optional<protocol::Location> result;
        shard_it->second.find_relations(hash,
                                        RelationKind::Definition,
                                        [&](const auto&, protocol::Range range) {
                                            result = protocol::Location{uri->str(), range};
                                            return false;
                                        });
        if(result)
            return result;
    }

    return std::nullopt;
}

std::optional<SymbolInfo>
    Indexer::resolve_hierarchy_item(const std::string& uri,
                                    llvm::StringRef path,
                                    const protocol::Range& range,
                                    const std::optional<protocol::LSPAny>& data,
                                    Session* session) {
    if(data) {
        if(auto* int_val = std::get_if<std::int64_t>(&*data)) {
            auto hash = static_cast<index::SymbolHash>(*int_val);
            std::string name;
            SymbolKind kind;
            if(find_symbol_info(hash, name, kind)) {
                return SymbolInfo{hash, std::move(name), kind, uri, range};
            }
        }
    }
    return lookup_symbol(uri, path, range.start, session);
}

void Indexer::collect_grouped_relations(
    index::SymbolHash hash,
    RelationKind kind,
    llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>>& target_ranges) {
    auto sym_it = workspace.project_index.symbols.find(hash);
    if(sym_it != workspace.project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(is_proj_path_open(file_id))
                continue;
            auto shard_it = workspace.merged_indices.find(file_id);
            if(shard_it == workspace.merged_indices.end())
                continue;
            shard_it->second.find_relations(hash, kind, [&](const auto& r, protocol::Range range) {
                target_ranges[r.target_symbol].push_back(range);
                return true;
            });
        }
    }
    for(auto& [_, sess]: sessions) {
        if(!sess.file_index)
            continue;
        sess.file_index->find_relations(hash, kind, [&](const auto& r, protocol::Range range) {
            target_ranges[r.target_symbol].push_back(range);
            return true;
        });
    }
}

void Indexer::collect_unique_targets(index::SymbolHash hash,
                                     RelationKind kind,
                                     llvm::SmallVectorImpl<index::SymbolHash>& targets) {
    llvm::DenseSet<index::SymbolHash> seen;
    auto sym_it = workspace.project_index.symbols.find(hash);
    if(sym_it != workspace.project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(is_proj_path_open(file_id))
                continue;
            auto shard_it = workspace.merged_indices.find(file_id);
            if(shard_it == workspace.merged_indices.end())
                continue;
            /// No position conversion needed -- just collect target symbol hashes.
            shard_it->second.index.lookup(hash, kind, [&](const index::Relation& r) {
                if(seen.insert(r.target_symbol).second) {
                    targets.push_back(r.target_symbol);
                }
                return true;
            });
        }
    }
    for(auto& [_, sess]: sessions) {
        if(!sess.file_index)
            continue;
        auto rel_it = sess.file_index->file_index.relations.find(hash);
        if(rel_it == sess.file_index->file_index.relations.end())
            continue;
        for(auto& r: rel_it->second) {
            if(r.kind & kind) {
                if(seen.insert(r.target_symbol).second) {
                    targets.push_back(r.target_symbol);
                }
            }
        }
    }
}

/// Resolve a symbol hash into a SymbolInfo with definition location.
/// Returns nullopt if the symbol or its definition cannot be found.
std::optional<SymbolInfo> Indexer::resolve_symbol(index::SymbolHash hash) {
    std::string name;
    SymbolKind kind;
    if(!find_symbol_info(hash, name, kind))
        return std::nullopt;
    auto def_loc = find_definition_location(hash);
    if(!def_loc)
        return std::nullopt;
    return SymbolInfo{hash, std::move(name), kind, def_loc->uri, def_loc->range};
}

std::vector<protocol::CallHierarchyIncomingCall>
    Indexer::find_incoming_calls(index::SymbolHash hash) {
    llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>> caller_ranges;
    collect_grouped_relations(hash, RelationKind::Caller, caller_ranges);

    std::vector<protocol::CallHierarchyIncomingCall> results;
    for(auto& [caller_hash, ranges]: caller_ranges) {
        auto info = resolve_symbol(caller_hash);
        if(!info)
            continue;
        results.push_back({build_call_hierarchy_item(*info), std::move(ranges)});
    }
    return results;
}

std::vector<protocol::CallHierarchyOutgoingCall>
    Indexer::find_outgoing_calls(index::SymbolHash hash) {
    llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>> callee_ranges;
    collect_grouped_relations(hash, RelationKind::Callee, callee_ranges);

    std::vector<protocol::CallHierarchyOutgoingCall> results;
    for(auto& [callee_hash, ranges]: callee_ranges) {
        auto info = resolve_symbol(callee_hash);
        if(!info)
            continue;
        results.push_back({build_call_hierarchy_item(*info), std::move(ranges)});
    }
    return results;
}

std::vector<protocol::TypeHierarchyItem> Indexer::find_supertypes(index::SymbolHash hash) {
    llvm::SmallVector<index::SymbolHash> base_hashes;
    collect_unique_targets(hash, RelationKind::Base, base_hashes);

    std::vector<protocol::TypeHierarchyItem> results;
    for(auto target_hash: base_hashes) {
        auto info = resolve_symbol(target_hash);
        if(!info)
            continue;
        results.push_back(build_type_hierarchy_item(*info));
    }
    return results;
}

std::vector<protocol::TypeHierarchyItem> Indexer::find_subtypes(index::SymbolHash hash) {
    llvm::SmallVector<index::SymbolHash> derived_hashes;
    collect_unique_targets(hash, RelationKind::Derived, derived_hashes);

    std::vector<protocol::TypeHierarchyItem> results;
    for(auto target_hash: derived_hashes) {
        auto info = resolve_symbol(target_hash);
        if(!info)
            continue;
        results.push_back(build_type_hierarchy_item(*info));
    }
    return results;
}

std::vector<protocol::SymbolInformation> Indexer::search_symbols(llvm::StringRef query,
                                                                 std::size_t max_results) {
    std::string query_lower = query.lower();

    auto is_indexable_kind = [](SymbolKind sk) {
        return sk == SymbolKind::Namespace || sk == SymbolKind::Class || sk == SymbolKind::Struct ||
               sk == SymbolKind::Union || sk == SymbolKind::Enum || sk == SymbolKind::Type ||
               sk == SymbolKind::Field || sk == SymbolKind::EnumMember ||
               sk == SymbolKind::Function || sk == SymbolKind::Method ||
               sk == SymbolKind::Variable || sk == SymbolKind::Parameter ||
               sk == SymbolKind::Macro || sk == SymbolKind::Concept || sk == SymbolKind::Module ||
               sk == SymbolKind::Operator || sk == SymbolKind::MacroParameter ||
               sk == SymbolKind::Label || sk == SymbolKind::Attribute;
    };

    auto matches_query = [&](llvm::StringRef name) {
        if(query_lower.empty())
            return true;
        return llvm::StringRef(name).lower().find(query_lower) != std::string::npos;
    };

    std::vector<protocol::SymbolInformation> results;
    llvm::DenseSet<index::SymbolHash> seen;

    for(auto& [hash, symbol]: workspace.project_index.symbols) {
        if(results.size() >= max_results)
            break;
        if(!is_indexable_kind(symbol.kind) || symbol.name.empty())
            continue;
        if(!matches_query(symbol.name))
            continue;
        auto def_loc = find_definition_location(hash);
        if(!def_loc)
            continue;

        protocol::SymbolInformation info;
        info.name = symbol.name;
        info.kind = to_lsp_symbol_kind(symbol.kind);
        info.location = std::move(*def_loc);
        results.push_back(std::move(info));
        seen.insert(hash);
    }

    for(auto& [_, sess]: sessions) {
        if(results.size() >= max_results)
            break;
        if(!sess.file_index)
            continue;
        for(auto& [hash, symbol]: sess.file_index->symbols) {
            if(results.size() >= max_results)
                break;
            if(seen.contains(hash))
                continue;
            if(!is_indexable_kind(symbol.kind) || symbol.name.empty())
                continue;
            if(!matches_query(symbol.name))
                continue;
            auto def_loc = find_definition_location(hash);
            if(!def_loc)
                continue;

            protocol::SymbolInformation info;
            info.name = symbol.name;
            info.kind = to_lsp_symbol_kind(symbol.kind);
            info.location = std::move(*def_loc);
            results.push_back(std::move(info));
            seen.insert(hash);
        }
    }
    return results;
}

protocol::SymbolKind Indexer::to_lsp_symbol_kind(SymbolKind kind) {
    switch(kind) {
        case SymbolKind::Namespace: return protocol::SymbolKind::Namespace;
        case SymbolKind::Class: return protocol::SymbolKind::Class;
        case SymbolKind::Struct: return protocol::SymbolKind::Struct;
        case SymbolKind::Union: return protocol::SymbolKind::Class;
        case SymbolKind::Enum: return protocol::SymbolKind::Enum;
        case SymbolKind::Type: return protocol::SymbolKind::TypeParameter;
        case SymbolKind::Field: return protocol::SymbolKind::Field;
        case SymbolKind::EnumMember: return protocol::SymbolKind::EnumMember;
        case SymbolKind::Function: return protocol::SymbolKind::Function;
        case SymbolKind::Method: return protocol::SymbolKind::Method;
        case SymbolKind::Variable: return protocol::SymbolKind::Variable;
        case SymbolKind::Parameter: return protocol::SymbolKind::Variable;
        case SymbolKind::Macro: return protocol::SymbolKind::Function;
        case SymbolKind::Concept: return protocol::SymbolKind::Interface;
        case SymbolKind::Module: return protocol::SymbolKind::Module;
        case SymbolKind::Operator: return protocol::SymbolKind::Operator;
        default: return protocol::SymbolKind::Variable;
    }
}

protocol::CallHierarchyItem Indexer::build_call_hierarchy_item(const SymbolInfo& info) {
    protocol::CallHierarchyItem item;
    item.name = info.name;
    item.kind = to_lsp_symbol_kind(info.kind);
    item.uri = info.uri;
    item.range = info.range;
    item.selection_range = info.range;
    item.data = protocol::LSPAny(static_cast<std::int64_t>(info.hash));
    return item;
}

protocol::TypeHierarchyItem Indexer::build_type_hierarchy_item(const SymbolInfo& info) {
    protocol::TypeHierarchyItem item;
    item.name = info.name;
    item.kind = to_lsp_symbol_kind(info.kind);
    item.uri = info.uri;
    item.range = info.range;
    item.selection_range = info.range;
    item.data = protocol::LSPAny(static_cast<std::int64_t>(info.hash));
    return item;
}

void Indexer::enqueue(std::uint32_t server_path_id) {
    index_queue.push_back(server_path_id);
}

void Indexer::pause_indexing() {
    ++pause_depth;
    if(pause_depth == 1) {
        resume_event.reset();
        LOG_DEBUG("Background indexing paused");
    }
}

void Indexer::resume_indexing() {
    if(pause_depth > 0)
        --pause_depth;
    if(pause_depth == 0) {
        resume_event.set();
        LOG_DEBUG("Background indexing resumed");
    }
}

void Indexer::schedule() {
    if(!*workspace.config.project.enable_indexing || indexing_active || indexing_scheduled)
        return;
    indexing_scheduled = true;

    if(!index_idle_timer) {
        index_idle_timer = std::make_shared<kota::timer>(kota::timer::create(loop));
    }
    index_idle_timer->start(std::chrono::milliseconds(*workspace.config.project.idle_timeout_ms));
    loop.schedule(run_background_indexing());
}

kota::task<> Indexer::index_one(std::uint32_t server_path_id) {
    auto file_path = std::string(workspace.path_pool.resolve(server_path_id));

    if(sessions.contains(server_path_id))
        co_return;

    if(!need_update(file_path))
        co_return;

    // For module interface units, compile their PCM (and transitive deps)
    // first so the stateless worker has the artifacts it needs.
    if(workspace.compile_graph && workspace.path_to_module.contains(server_path_id)) {
        co_await workspace.compile_graph->compile(server_path_id);
    }

    worker::BuildParams params;
    params.kind = worker::BuildKind::Index;
    params.file = file_path;
    if(!compiler.fill_compile_args(file_path, params.directory, params.arguments, nullptr))
        co_return;

    workspace.fill_pcm_deps(params.pcms);

    LOG_INFO("Background indexing: {}", file_path);

    auto result = co_await pool.send_stateless(params);
    if(result.has_value() && result.value().success && !result.value().tu_index_data.empty()) {
        LOG_INFO("Background indexing got TUIndex for {}: {} bytes",
                 file_path,
                 result.value().tu_index_data.size());
        merge(result.value().tu_index_data.data(), result.value().tu_index_data.size());
    } else if(result.has_value() && !result.value().success) {
        LOG_WARN("Background index failed for {}: {}", file_path, result.value().error);
    } else if(result.has_value() && result.value().tu_index_data.empty()) {
        LOG_WARN("Background index returned empty TUIndex for {}", file_path);
    } else {
        LOG_WARN("Background index IPC error for {}: {}", file_path, result.error().message);
    }
}

kota::task<> Indexer::monitor_resources(std::uint32_t generation) {
    while(generation == monitor_generation) {
        co_await kota::sleep(std::chrono::milliseconds(3000), loop);

        if(generation != monitor_generation)
            break;

        auto mem = kota::sys::memory();
        if(mem.total == 0)
            continue;

        // Respect cgroup/container limits when present.
        auto effective_total =
            (mem.constrained > 0 && mem.constrained < mem.total) ? mem.constrained : mem.total;
        auto ratio = static_cast<double>(mem.available) / static_cast<double>(effective_total);

        if(ratio < 0.15 && max_concurrent > 1) {
            --max_concurrent;
            LOG_INFO("Index concurrency -> {} (memory pressure: {:.0f}% available)",
                     max_concurrent,
                     ratio * 100);
        } else if(ratio > 0.30 && max_concurrent < baseline_concurrent) {
            ++max_concurrent;
            LOG_DEBUG("Index concurrency -> {} (memory OK: {:.0f}% available)",
                      max_concurrent,
                      ratio * 100);
        }
    }
}

kota::task<> Indexer::run_background_indexing() {
    if(index_idle_timer) {
        co_await index_idle_timer->wait();
    }
    indexing_scheduled = false;

    if(index_queue_pos >= index_queue.size()) {
        LOG_DEBUG("Background indexing: queue exhausted");
        co_return;
    }

    indexing_active = true;
    ++monitor_generation;
    loop.schedule(monitor_resources(monitor_generation));

    // Put module interface units first so their PCMs are built before
    // non-module files that might import them.
    std::stable_partition(
        index_queue.begin() + index_queue_pos,
        index_queue.end(),
        [this](std::uint32_t id) { return workspace.path_to_module.contains(id); });

    auto batch = index_queue.size() - index_queue_pos;
    std::size_t dispatched = 0;
    std::size_t completed = 0;
    finished = 0;

    // Progress reporting via LSP $/progress.
    std::optional<lsp::ProgressReporter<kota::ipc::JsonPeer>> progress;
    if(peer) {
        progress.emplace(*peer, protocol::ProgressToken(std::string("clice/backgroundIndex")));
        auto create_result = co_await progress->create();
        if(!create_result.has_error()) {
            progress->begin("Indexing", std::format("0/{} files", batch), 0);
        } else {
            progress.reset();
        }
    }

    while(index_queue_pos < index_queue.size() || inflight > 0) {
        // Dispatch new tasks up to max_concurrent.
        while(index_queue_pos < index_queue.size() && inflight < max_concurrent) {
            // Wait if paused by a user request.
            if(pause_depth > 0) {
                co_await resume_event.wait();
            }

            auto server_path_id = index_queue[index_queue_pos++];

            // Quick pre-filter: skip open files and fresh files without
            // consuming a concurrency slot.
            auto file_path = std::string(workspace.path_pool.resolve(server_path_id));
            if(sessions.contains(server_path_id) || !need_update(file_path)) {
                ++completed;
                continue;
            }

            ++inflight;
            ++dispatched;

            // Launch the index task.  On completion it decrements
            // inflight, bumps finished, and signals the event.
            loop.schedule([](Indexer* self, std::uint32_t id, kota::event& done) -> kota::task<> {
                co_await self->index_one(id);
                --self->inflight;
                ++self->finished;
                done.set();
            }(this, server_path_id, completion_event));
        }

        if(inflight == 0)
            break;

        // Wait for at least one task to finish.
        co_await completion_event.wait();
        completion_event.reset();

        // Drain all completions that occurred since last wake.
        completed += std::exchange(finished, 0);

        // Report progress.
        if(progress) {
            auto pct = batch > 0 ? static_cast<std::uint32_t>(completed * 100 / batch) : 100;
            progress->report(std::format("{}/{} files", completed, batch), pct);
        }
    }

    if(progress) {
        progress->end(std::format("Indexed {} files", dispatched));
    }

    indexing_active = false;
    ++monitor_generation;  // Stop the monitor coroutine.
    LOG_INFO("Background indexing complete: {} files dispatched", dispatched);
    save(workspace.config.project.index_dir);
}

}  // namespace clice
