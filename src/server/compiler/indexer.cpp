#include "server/compiler/indexer.h"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

#include "index/tu_index.h"
#include "server/compiler/compiler.h"
#include "server/protocol/worker.h"
#include "server/service/session.h"
#include "server/worker/worker_pool.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/uri.h"
#include "llvm/ADT/DenseSet.h"
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
            shard.merge(global_path_id,
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
            shard.merge(global_path_id, *include_id, file_idx, header_content);
        }
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

/// Begin a two-phase store write and serialize the blob to its tmp path.
/// Returns the entry to commit, or nullopt if serialization failed.
static std::optional<CacheStore::PendingEntry>
    serialize_blob(CacheStore& store,
                   llvm::StringRef key,
                   llvm::function_ref<void(llvm::raw_ostream&)> serialize) {
    auto pending = store.begin_store("index", key);
    std::error_code ec;
    llvm::raw_fd_ostream os(pending.tmp_path, ec);
    if(ec) {
        LOG_WARN("Failed to write index blob {}: {}", key, ec.message());
        store.abort(pending);
        return std::nullopt;
    }
    serialize(os);
    os.flush();
    // A truncated blob (disk full) must never be committed: the index
    // namespace is Persistent, so it would be served forever.
    if(os.has_error()) {
        LOG_WARN("Failed to write index blob {}: {}", key, os.error().message());
        os.clear_error();
        store.abort(pending);
        return std::nullopt;
    }
    return pending;
}

kota::task<> Indexer::save() {
    if(!workspace.store)
        co_return;
    auto& store = *workspace.store;

    // Phase 1, synchronous: serialize the ProjectIndex and every dirty
    // shard to tmp files.  No suspension point in between, so the batch is
    // a consistent snapshot even if a merge runs before the commits below
    // are done.
    llvm::SmallVector<CacheStore::PendingEntry> batch;
    if(auto pending = serialize_blob(store, "project", [&](llvm::raw_ostream& os) {
           workspace.project_index.serialize(os);
       })) {
        batch.push_back(std::move(*pending));
        LOG_INFO("Saved ProjectIndex ({} symbols)", workspace.project_index.symbols.size());
    }

    std::size_t saved = 0;
    std::size_t total = workspace.merged_indices.size();
    for(auto& [path_id, shard]: workspace.merged_indices) {
        if(!shard.need_rewrite())
            continue;
        if(auto pending =
               serialize_blob(store, std::to_string(path_id), [&](llvm::raw_ostream& os) {
                   shard.serialize(os);
               })) {
            batch.push_back(std::move(*pending));
            ++saved;
        }
    }
    LOG_INFO("Saved {} MergedIndex shards (of {} total)", saved, total);

    // Phase 2: commit each blob (fsync + atomic rename) on the kota thread
    // pool, keeping the heavy IO off the event loop.
    for(auto& pending: batch) {
        auto key = pending.key;
        auto committed = co_await kota::queue([&] { return store.commit(std::move(pending)); });
        if(!committed.has_value() || !committed.value().has_value()) {
            LOG_WARN("Failed to commit index blob {}", key);
        }
    }
}

void Indexer::load() {
    if(!workspace.store)
        return;

    bool has_project = false;
    auto project_path = workspace.store->lookup("index", "project");
    if(project_path) {
        auto buf = llvm::MemoryBuffer::getFile(*project_path);
        if(!buf) {
            // Transient read failure — don't load shards (useless without
            // the project index), but don't destroy them either.
            LOG_WARN("Failed to read ProjectIndex blob: {}", buf.getError().message());
            return;
        }
        workspace.project_index = index::ProjectIndex::from((*buf)->getBufferStart());
        has_project = true;
        LOG_INFO("Loaded ProjectIndex: {} symbols", workspace.project_index.symbols.size());
    }

    // Load shards; sweep blobs that no longer correspond to anything —
    // unparseable keys, or all shards when the project index itself is
    // gone (Persistent namespace cleanup is the caller's mark-and-sweep).
    llvm::SmallVector<std::string> orphans;
    workspace.store->for_each_key("index", [&](llvm::StringRef key) {
        if(key == "project")
            return;

        std::uint32_t path_id = 0;
        if(key.getAsInteger(10, path_id) || !has_project) {
            orphans.push_back(key.str());
            return;
        }

        auto shard_path = workspace.store->lookup("index", key);
        if(shard_path) {
            workspace.merged_indices[path_id] = index::MergedIndex::load(*shard_path);
        }
    });

    for(auto& key: orphans) {
        workspace.store->invalidate("index", key);
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
    return merged_it->second.need_update(path_mapping);
}

bool Indexer::find_symbol_info(index::SymbolHash hash, std::string& name, SymbolKind& kind) const {
    bool found = false;
    foreach_session([&](std::uint32_t, const Session& session) -> bool {
        auto it = session.symbols->find(hash);
        if(it != session.symbols->end()) {
            name = it->second.name;
            kind = it->second.kind;
            found = true;
            return false;
        }
        return true;
    });
    if(found)
        return true;
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
    // FIXME: when ast_dirty, we fall back to MergedIndex which may be staler.
    // Consider awaiting the pending recompilation to serve fresher results.
    if(session && session->file_index && !session->ast_dirty) {
        auto map = session->line_map();
        auto offset = map.to_offset(position);
        if(!offset)
            return {};
        CursorHit hit;
        session->file_index->lookup(*offset, [&](const index::Occurrence& occ) {
            auto range = map.to_range(occ.range.begin, occ.range.end);
            if(range) {
                hit = {occ.target, *range};
                return false;
            }
            return true;
        });
        return hit;
    }

    // Fallback to MergedIndex, using session text for position -> offset.
    if(!session)
        return {};
    auto offset = session->line_map().to_offset(position);
    if(!offset)
        return {};

    auto proj_it = workspace.project_index.path_pool.find(path);
    if(proj_it == workspace.project_index.path_pool.cache.end())
        return {};
    auto shard_it = workspace.merged_indices.find(proj_it->second);
    if(shard_it == workspace.merged_indices.end())
        return {};

    auto& merged_index = shard_it->second;
    auto ls = merged_index.line_starts();
    if(ls.empty())
        return {};
    lsp::LineMap map(merged_index.content(), ls);
    CursorHit hit;
    merged_index.lookup(*offset, [&](const index::Occurrence& o) {
        auto range = map.to_range(o.range.begin, o.range.end);
        if(range)
            hit = {o.target, *range};
        return false;
    });
    return hit;
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
            auto& merged_index = shard_it->second;
            auto ls = merged_index.line_starts();
            if(ls.empty())
                continue;
            lsp::LineMap map(merged_index.content(), ls);
            merged_index.lookup(hit.hash, kind, [&](const index::Relation& r) {
                if(auto range = map.to_range(r.range.begin, r.range.end))
                    locations.push_back({uri->str(), *range});
                return true;
            });
        }
    }

    foreach_session([&](std::uint32_t id, const Session& session) -> bool {
        auto uri = lsp::URI::from_file_path(std::string(workspace.path_pool.resolve(id)));
        if(!uri)
            return true;
        auto map = session.line_map();
        session.file_index->lookup(hit.hash, kind, [&](const index::Relation& r) {
            if(auto range = map.to_range(r.range.begin, r.range.end))
                locations.push_back({uri->str(), *range});
            return true;
        });
        return true;
    });

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
    std::optional<protocol::Location> session_result;
    foreach_session([&](std::uint32_t id, const Session& session) -> bool {
        auto uri = lsp::URI::from_file_path(std::string(workspace.path_pool.resolve(id)));
        if(!uri)
            return true;
        auto map = session.line_map();
        session.file_index->lookup(hash, RelationKind::Definition, [&](const index::Relation& r) {
            if(auto range = map.to_range(r.range.begin, r.range.end)) {
                session_result = protocol::Location{uri->str(), *range};
                return false;
            }
            return true;
        });
        return !session_result.has_value();
    });
    if(session_result)
        return session_result;

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
        auto& merged_index = shard_it->second;
        auto ls = merged_index.line_starts();
        if(ls.empty())
            continue;
        lsp::LineMap map(merged_index.content(), ls);
        std::optional<protocol::Location> result;
        merged_index.lookup(hash, RelationKind::Definition, [&](const index::Relation& r) {
            if(auto range = map.to_range(r.range.begin, r.range.end)) {
                result = protocol::Location{uri->str(), *range};
                return false;
            }
            return true;
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
            auto& merged_index = shard_it->second;
            auto ls = merged_index.line_starts();
            if(ls.empty())
                continue;
            lsp::LineMap map(merged_index.content(), ls);
            merged_index.lookup(hash, kind, [&](const index::Relation& r) {
                if(auto range = map.to_range(r.range.begin, r.range.end))
                    target_ranges[r.target_symbol].push_back(*range);
                return true;
            });
        }
    }
    foreach_session([&](std::uint32_t, const Session& session) -> bool {
        auto map = session.line_map();
        session.file_index->lookup(hash, kind, [&](const index::Relation& r) {
            if(auto range = map.to_range(r.range.begin, r.range.end))
                target_ranges[r.target_symbol].push_back(*range);
            return true;
        });
        return true;
    });
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
            shard_it->second.lookup(hash, kind, [&](const index::Relation& r) {
                if(seen.insert(r.target_symbol).second) {
                    targets.push_back(r.target_symbol);
                }
                return true;
            });
        }
    }
    foreach_session([&](std::uint32_t, const Session& session) -> bool {
        auto rel_it = session.file_index->relations.find(hash);
        if(rel_it == session.file_index->relations.end())
            return true;
        for(auto& r: rel_it->second) {
            if(r.kind & kind) {
                if(seen.insert(r.target_symbol).second) {
                    targets.push_back(r.target_symbol);
                }
            }
        }
        return true;
    });
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

static std::string extract_line(llvm::StringRef content, std::uint32_t offset) {
    if(content.empty() || offset >= content.size())
        return {};
    std::size_t line_start = 0;
    if(offset > 0) {
        auto pos = content.rfind('\n', offset - 1);
        if(pos != llvm::StringRef::npos)
            line_start = pos + 1;
    }
    auto line_end = content.find('\n', offset);
    if(line_end == llvm::StringRef::npos)
        line_end = content.size();
    return content.slice(line_start, line_end).str();
}

std::optional<Indexer::DefinitionText> Indexer::get_definition_text(index::SymbolHash hash) {
    std::optional<DefinitionText> session_result;
    foreach_session([&](std::uint32_t id, const Session& session) -> bool {
        auto map = session.line_map();
        session.file_index->lookup(hash, RelationKind::Definition, [&](const index::Relation& rel) {
            auto def_range = std::bit_cast<LocalSourceRange>(rel.target_symbol);
            if(def_range.begin >= def_range.end || def_range.end > session.text.size())
                return true;
            auto range = map.to_range(def_range.begin, def_range.end);
            if(!range)
                return true;
            session_result = DefinitionText{
                .file = std::string(workspace.path_pool.resolve(id)),
                .start_line = static_cast<int>(range->start.line) + 1,
                .end_line = static_cast<int>(range->end.line) + 1,
                .text = std::string(
                    session.text.substr(def_range.begin, def_range.end - def_range.begin)),
            };
            return false;
        });
        return !session_result.has_value();
    });
    if(session_result)
        return session_result;

    auto sym_it = workspace.project_index.symbols.find(hash);
    if(sym_it == workspace.project_index.symbols.end())
        return std::nullopt;

    for(auto file_id: sym_it->second.reference_files) {
        if(is_proj_path_open(file_id))
            continue;
        auto shard_it = workspace.merged_indices.find(file_id);
        if(shard_it == workspace.merged_indices.end())
            continue;
        auto& merged_index = shard_it->second;
        auto ls = merged_index.line_starts();
        if(ls.empty())
            continue;
        auto content = merged_index.content();
        lsp::LineMap map(content, ls);

        std::optional<DefinitionText> result;
        merged_index.lookup(hash, RelationKind::Definition, [&](const index::Relation& r) {
            auto def_range = std::bit_cast<LocalSourceRange>(r.target_symbol);
            if(def_range.begin >= def_range.end || def_range.end > content.size())
                return true;
            auto range = map.to_range(def_range.begin, def_range.end);
            if(!range)
                return true;
            result = DefinitionText{
                .file = workspace.project_index.path_pool.path(file_id).str(),
                .start_line = static_cast<int>(range->start.line) + 1,
                .end_line = static_cast<int>(range->end.line) + 1,
                .text =
                    std::string(content.substr(def_range.begin, def_range.end - def_range.begin)),
            };
            return false;
        });
        if(result)
            return result;
    }

    return std::nullopt;
}

std::vector<Indexer::ReferenceWithContext> Indexer::collect_references(index::SymbolHash hash,
                                                                       RelationKind kind) {
    std::vector<ReferenceWithContext> results;

    auto sym_it = workspace.project_index.symbols.find(hash);
    if(sym_it != workspace.project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(is_proj_path_open(file_id))
                continue;
            auto shard_it = workspace.merged_indices.find(file_id);
            if(shard_it == workspace.merged_indices.end())
                continue;
            auto& merged_index = shard_it->second;
            auto ls = merged_index.line_starts();
            if(ls.empty())
                continue;
            auto content = merged_index.content();
            lsp::LineMap map(content, ls);
            auto file_path = workspace.project_index.path_pool.path(file_id);

            merged_index.lookup(hash, kind, [&](const index::Relation& r) {
                auto pos = map.to_position(r.range.begin);
                if(!pos)
                    return true;
                results.push_back(ReferenceWithContext{
                    .file = file_path.str(),
                    .line = static_cast<int>(pos->line) + 1,
                    .context = extract_line(content, r.range.begin),
                });
                return true;
            });
        }
    }

    foreach_session([&](std::uint32_t id, const Session& session) -> bool {
        auto map = session.line_map();
        auto file_path = workspace.path_pool.resolve(id);
        session.file_index->lookup(hash, kind, [&](const index::Relation& rel) {
            auto pos = map.to_position(rel.range.begin);
            if(!pos)
                return true;
            results.push_back(ReferenceWithContext{
                .file = file_path.str(),
                .line = static_cast<int>(pos->line) + 1,
                .context = extract_line(session.text, rel.range.begin),
            });
            return true;
        });
        return true;
    });

    return results;
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

    foreach_session([&](std::uint32_t, const Session& session) -> bool {
        if(results.size() >= max_results)
            return false;
        for(auto& [hash, symbol]: *session.symbols) {
            if(results.size() >= max_results)
                return false;
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
        return true;
    });
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

kota::task<> Indexer::stop() {
    bg_tasks.cancel();
    co_await bg_tasks.join();
}

void Indexer::schedule() {
    if(!*workspace.config.project.enable_indexing || indexing_active || indexing_scheduled)
        return;
    indexing_scheduled = true;

    if(!index_idle_timer) {
        index_idle_timer = std::make_shared<kota::timer>(kota::timer::create(loop));
    }
    index_idle_timer->start(std::chrono::milliseconds(*workspace.config.project.idle_timeout_ms));

    if(!bg_tasks.spawn(run_background_indexing())) {
        indexing_scheduled = false;
        LOG_WARN("Failed to spawn background indexing task (task group stopped)");
    }
}

kota::task<> Indexer::index_one(std::uint32_t server_path_id,
                                std::size_t index,
                                std::size_t total) {
    auto file_path = std::string(workspace.path_pool.resolve(server_path_id));

    if(is_open && is_open(server_path_id))
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

    LOG_INFO("[{}/{}] Indexing {}", index, total, file_path);

    auto result = co_await pool.send_stateless(params);
    if(result.has_value() && result.value().success && !result.value().tu_index_data.empty()) {
        LOG_INFO("[{}/{}] Indexed {}: {} bytes",
                 index,
                 total,
                 file_path,
                 result.value().tu_index_data.size());
        merge(result.value().tu_index_data.data(), result.value().tu_index_data.size());
    } else if(result.has_value() && !result.value().success) {
        LOG_WARN("[{}/{}] Index failed for {}: {}", index, total, file_path, result.value().error);
    } else if(result.has_value() && result.value().tu_index_data.empty()) {
        LOG_WARN("[{}/{}] Index returned empty TUIndex for {}", index, total, file_path);
    } else {
        LOG_WARN("[{}/{}] Index IPC error for {}: {}",
                 index,
                 total,
                 file_path,
                 result.error().message);
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
    LOG_DEBUG("Background indexing: starting, {} files queued",
              index_queue.size() - index_queue_pos);

    std::stable_partition(
        index_queue.begin() + index_queue_pos,
        index_queue.end(),
        [this](std::uint32_t id) { return workspace.path_to_module.contains(id); });

    auto total = index_queue.size() - index_queue_pos;
    std::size_t dispatched = 0;
    std::size_t completed = 0;

    std::optional<lsp::ProgressReporter<kota::ipc::JsonPeer>> progress;
    if(peer) {
        progress.emplace(*peer, protocol::ProgressToken(std::string("clice/backgroundIndex")));
        // Timeout prevents indexing from hanging when the client never responds.
        auto create_result =
            co_await progress->create({.timeout = std::chrono::milliseconds(3000)});
        if(!create_result.has_error()) {
            progress->begin("Indexing", std::format("0/{} files", total), 0);
        } else {
            progress.reset();
        }
    }

    kota::task_group<> workers(loop);

    while(index_queue_pos < index_queue.size()) {
        if(pause_depth > 0)
            co_await resume_event.wait();

        auto server_path_id = index_queue[index_queue_pos++];
        auto file_path = std::string(workspace.path_pool.resolve(server_path_id));
        if((is_open && is_open(server_path_id)) || !need_update(file_path)) {
            ++completed;
            continue;
        }

        ++dispatched;
        workers.spawn([&, server_path_id, n = dispatched]() -> kota::task<> {
            co_await index_one(server_path_id, n, total);
            ++completed;
            if(progress) {
                auto pct = total > 0 ? static_cast<std::uint32_t>(completed * 100 / total) : 100;
                progress->report(std::format("{}/{} files", completed, total), pct);
            }
        }());
    }

    LOG_DEBUG("Background indexing: all {} tasks spawned, waiting for completion", dispatched);
    co_await workers.join();

    if(progress) {
        progress->end(std::format("Indexed {} files", dispatched));
    }

    indexing_active = false;
    LOG_INFO("Background indexing complete: {} files dispatched", dispatched);
    co_await save();
}

}  // namespace clice
