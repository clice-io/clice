#include "server/indexer.h"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/protocol.h"
#include "eventide/ipc/lsp/uri.h"
#include "index/tu_index.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace clice {

namespace lsp = eventide::ipc::lsp;

/// Find the tightest (innermost) occurrence containing `offset` via binary search.
const static index::Occurrence* lookup_occurrence(const std::vector<index::Occurrence>& occs,
                                                  std::uint32_t offset) {
    auto it = std::ranges::lower_bound(occs, offset, {}, [](const index::Occurrence& o) {
        return o.range.end;
    });
    const index::Occurrence* best = nullptr;
    while(it != occs.end() && it->range.contains(offset)) {
        if(!best || (it->range.end - it->range.begin) < (best->range.end - best->range.begin)) {
            best = &*it;
        }
        ++it;
    }
    return best;
}

// ── OpenFileIndex ────────────────────────────────────────────────────────

std::optional<std::pair<index::SymbolHash, protocol::Range>>
    OpenFileIndex::find_occurrence(std::uint32_t offset) const {
    if(!mapper)
        return std::nullopt;
    auto* occ = lookup_occurrence(file_index.occurrences, offset);
    if(!occ)
        return std::nullopt;
    auto start = mapper->to_position(occ->range.begin);
    auto end = mapper->to_position(occ->range.end);
    if(!start || !end)
        return std::nullopt;
    return std::pair{
        occ->target,
        protocol::Range{*start, *end}
    };
}

// ── MergedIndexShard ─────────────────────────────────────────────────────

std::optional<std::pair<index::SymbolHash, protocol::Range>>
    MergedIndexShard::find_occurrence(std::uint32_t offset) const {
    auto* m = mapper();
    if(!m)
        return std::nullopt;
    std::optional<std::pair<index::SymbolHash, protocol::Range>> result;
    index.lookup(offset, [&](const index::Occurrence& o) {
        auto start = m->to_position(o.range.begin);
        auto end = m->to_position(o.range.end);
        if(start && end) {
            result = {
                o.target,
                protocol::Range{*start, *end}
            };
        }
        return false;
    });
    return result;
}

// ── Indexer: data management ─────────────────────────────────────────────

void Indexer::merge(const void* tu_index_data, std::size_t size) {
    auto tu_index = index::TUIndex::from(tu_index_data);
    auto file_ids_map = project_index.merge(tu_index);
    auto main_tu_path_id = static_cast<std::uint32_t>(tu_index.graph.paths.size() - 1);

    auto merge_file_index = [&](std::uint32_t tu_path_id, index::FileIndex& file_idx) {
        auto global_path_id = file_ids_map[tu_path_id];
        auto& shard = merged_indices[global_path_id];

        if(tu_path_id == main_tu_path_id) {
            std::vector<index::IncludeLocation> include_locs;
            for(auto& loc: tu_index.graph.locations) {
                index::IncludeLocation remapped = loc;
                remapped.path_id = file_ids_map[loc.path_id];
                include_locs.push_back(remapped);
            }
            auto file_path = project_index.path_pool.path(global_path_id);
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
            std::uint32_t include_id = 0;
            for(std::uint32_t i = 0; i < tu_index.graph.locations.size(); ++i) {
                if(tu_index.graph.locations[i].path_id == tu_path_id) {
                    include_id = i;
                    break;
                }
            }
            auto header_path = project_index.path_pool.path(global_path_id);
            llvm::StringRef header_content;
            std::string header_content_storage;
            auto header_buf = llvm::MemoryBuffer::getFile(header_path);
            if(header_buf) {
                header_content_storage = (*header_buf)->getBuffer().str();
                header_content = header_content_storage;
            }
            shard.index.merge(global_path_id, include_id, file_idx, header_content);
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
             merged_indices.size());
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
            project_index.serialize(os);
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
    for(auto& [path_id, shard]: merged_indices) {
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
    LOG_INFO("Saved {} MergedIndex shards (of {} total)", saved, merged_indices.size());
}

void Indexer::load(llvm::StringRef index_dir) {
    if(index_dir.empty())
        return;

    auto project_path = path::join(index_dir, "project.idx");
    auto buf = llvm::MemoryBuffer::getFile(project_path);
    if(buf) {
        project_index = index::ProjectIndex::from((*buf)->getBufferStart());
        LOG_INFO("Loaded ProjectIndex: {} symbols", project_index.symbols.size());
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
        merged_indices[path_id] = MergedIndexShard{index::MergedIndex::load(it->path())};
    }

    if(!merged_indices.empty()) {
        LOG_INFO("Loaded {} MergedIndex shards", merged_indices.size());
    }
}

bool Indexer::need_update(llvm::StringRef file_path) {
    auto cache_it = project_index.path_pool.find(file_path);
    if(cache_it == project_index.path_pool.cache.end())
        return true;

    auto merged_it = merged_indices.find(cache_it->second);
    if(merged_it == merged_indices.end())
        return true;

    llvm::SmallVector<llvm::StringRef> path_mapping;
    for(auto& p: project_index.path_pool.paths) {
        path_mapping.push_back(p);
    }
    return merged_it->second.index.need_update(path_mapping);
}

void Indexer::set_open_file(std::uint32_t server_path_id,
                            llvm::StringRef file_path,
                            OpenFileIndex index) {
    auto& stored = (open_file_indices[server_path_id] = std::move(index));
    stored.mapper.emplace(stored.content, lsp::PositionEncoding::UTF16);
    auto proj_cache_it = project_index.path_pool.find(file_path);
    if(proj_cache_it != project_index.path_pool.cache.end()) {
        open_proj_path_ids.insert(proj_cache_it->second);
    }
}

void Indexer::remove_open_file(std::uint32_t server_path_id, llvm::StringRef file_path) {
    open_file_indices.erase(server_path_id);
    auto proj_cache_it = project_index.path_pool.find(file_path);
    if(proj_cache_it != project_index.path_pool.cache.end()) {
        open_proj_path_ids.erase(proj_cache_it->second);
    }
}

// ── Indexer: symbol queries ──────────────────────────────────────────────

bool Indexer::find_symbol_info(index::SymbolHash hash, std::string& name, SymbolKind& kind) const {
    for(auto& [_, index]: open_file_indices) {
        auto it = index.symbols.find(hash);
        if(it != index.symbols.end()) {
            name = it->second.name;
            kind = it->second.kind;
            return true;
        }
    }
    auto it = project_index.symbols.find(hash);
    if(it != project_index.symbols.end()) {
        name = it->second.name;
        kind = it->second.kind;
        return true;
    }
    return false;
}

Indexer::CursorHit Indexer::resolve_cursor(llvm::StringRef path,
                                           std::uint32_t server_path_id,
                                           const protocol::Position& position,
                                           const std::string* doc_text) {
    // Try open file index first.
    auto it = open_file_indices.find(server_path_id);
    if(it != open_file_indices.end()) {
        auto& index = it->second;
        if(!index.mapper)
            return {};
        auto offset = index.mapper->to_offset(position);
        if(!offset)
            return {};
        if(auto found = index.find_occurrence(*offset))
            return {found->first, found->second};
        return {};
    }

    // Fallback to MergedIndex, using doc_text for position → offset.
    if(!doc_text)
        return {};
    lsp::PositionMapper doc_mapper(*doc_text, lsp::PositionEncoding::UTF16);
    auto offset = doc_mapper.to_offset(position);
    if(!offset)
        return {};

    auto proj_it = project_index.path_pool.find(path);
    if(proj_it == project_index.path_pool.cache.end())
        return {};
    auto shard_it = merged_indices.find(proj_it->second);
    if(shard_it == merged_indices.end())
        return {};

    if(auto found = shard_it->second.find_occurrence(*offset))
        return {found->first, found->second};
    return {};
}

std::vector<protocol::Location> Indexer::query_relations(llvm::StringRef path,
                                                         std::uint32_t server_path_id,
                                                         const protocol::Position& position,
                                                         RelationKind kind,
                                                         const std::string* doc_text) {
    auto hit = resolve_cursor(path, server_path_id, position, doc_text);
    if(hit.hash == 0)
        return {};

    std::vector<protocol::Location> locations;

    auto sym_it = project_index.symbols.find(hit.hash);
    if(sym_it != project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(open_proj_path_ids.contains(file_id))
                continue;
            auto shard_it = merged_indices.find(file_id);
            if(shard_it == merged_indices.end())
                continue;
            auto uri = lsp::URI::from_file_path(project_index.path_pool.path(file_id));
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

    for(auto& [id, index]: open_file_indices) {
        auto uri = lsp::URI::from_file_path(std::string(path_pool.resolve(id)));
        if(!uri)
            continue;
        index.find_relations(hit.hash, kind, [&](const auto&, protocol::Range range) {
            locations.push_back({uri->str(), range});
            return true;
        });
    }

    return locations;
}

std::optional<SymbolInfo> Indexer::lookup_symbol(const std::string& uri,
                                                 llvm::StringRef path,
                                                 std::uint32_t server_path_id,
                                                 const protocol::Position& position,
                                                 const std::string* doc_text) {
    auto hit = resolve_cursor(path, server_path_id, position, doc_text);
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
    for(auto& [id, index]: open_file_indices) {
        auto uri = lsp::URI::from_file_path(std::string(path_pool.resolve(id)));
        if(!uri)
            continue;
        std::optional<protocol::Location> result;
        index.find_relations(hash,
                             RelationKind::Definition,
                             [&](const auto&, protocol::Range range) {
                                 result = protocol::Location{uri->str(), range};
                                 return false;
                             });
        if(result)
            return result;
    }

    // Fall back to ProjectIndex reference files.
    auto sym_it = project_index.symbols.find(hash);
    if(sym_it == project_index.symbols.end())
        return std::nullopt;

    for(auto file_id: sym_it->second.reference_files) {
        if(open_proj_path_ids.contains(file_id))
            continue;
        auto shard_it = merged_indices.find(file_id);
        if(shard_it == merged_indices.end())
            continue;
        auto uri = lsp::URI::from_file_path(project_index.path_pool.path(file_id));
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
                                    std::uint32_t server_path_id,
                                    const protocol::Range& range,
                                    const std::optional<protocol::LSPAny>& data,
                                    const std::string* doc_text) {
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
    return lookup_symbol(uri, path, server_path_id, range.start, doc_text);
}

// ── Indexer: relation collection helpers ─────────────────────────────────

void Indexer::collect_grouped_relations(
    index::SymbolHash hash,
    RelationKind kind,
    llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>>& target_ranges) {
    auto sym_it = project_index.symbols.find(hash);
    if(sym_it != project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(open_proj_path_ids.contains(file_id))
                continue;
            auto shard_it = merged_indices.find(file_id);
            if(shard_it == merged_indices.end())
                continue;
            shard_it->second.find_relations(hash, kind, [&](const auto& r, protocol::Range range) {
                target_ranges[r.target_symbol].push_back(range);
                return true;
            });
        }
    }
    for(auto& [_, index]: open_file_indices) {
        index.find_relations(hash, kind, [&](const auto& r, protocol::Range range) {
            target_ranges[r.target_symbol].push_back(range);
            return true;
        });
    }
}

void Indexer::collect_unique_targets(index::SymbolHash hash,
                                     RelationKind kind,
                                     llvm::SmallVectorImpl<index::SymbolHash>& targets) {
    llvm::DenseSet<index::SymbolHash> seen;
    auto sym_it = project_index.symbols.find(hash);
    if(sym_it != project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(open_proj_path_ids.contains(file_id))
                continue;
            auto shard_it = merged_indices.find(file_id);
            if(shard_it == merged_indices.end())
                continue;
            shard_it->second.find_relations(hash, kind, [&](const auto& r, protocol::Range) {
                if(seen.insert(r.target_symbol).second) {
                    targets.push_back(r.target_symbol);
                }
                return true;
            });
        }
    }
    for(auto& [_, index]: open_file_indices) {
        index.find_relations(hash, kind, [&](const auto& r, protocol::Range) {
            if(seen.insert(r.target_symbol).second) {
                targets.push_back(r.target_symbol);
            }
            return true;
        });
    }
}

// ── Indexer: hierarchy queries ───────────────────────────────────────────

std::vector<protocol::CallHierarchyIncomingCall>
    Indexer::find_incoming_calls(index::SymbolHash hash) {
    llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>> caller_ranges;
    collect_grouped_relations(hash, RelationKind::Caller, caller_ranges);

    std::vector<protocol::CallHierarchyIncomingCall> results;
    for(auto& [caller_hash, ranges]: caller_ranges) {
        auto def_loc = find_definition_location(caller_hash);
        if(!def_loc)
            continue;
        std::string name;
        SymbolKind kind;
        if(!find_symbol_info(caller_hash, name, kind))
            continue;

        protocol::CallHierarchyItem item;
        item.name = name;
        item.kind = to_lsp_symbol_kind(kind);
        item.uri = def_loc->uri;
        item.range = def_loc->range;
        item.selection_range = def_loc->range;
        item.data = protocol::LSPAny(static_cast<std::int64_t>(caller_hash));

        results.push_back({std::move(item), std::move(ranges)});
    }
    return results;
}

std::vector<protocol::CallHierarchyOutgoingCall>
    Indexer::find_outgoing_calls(index::SymbolHash hash) {
    llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>> callee_ranges;
    collect_grouped_relations(hash, RelationKind::Callee, callee_ranges);

    std::vector<protocol::CallHierarchyOutgoingCall> results;
    for(auto& [callee_hash, ranges]: callee_ranges) {
        auto def_loc = find_definition_location(callee_hash);
        if(!def_loc)
            continue;
        std::string name;
        SymbolKind kind;
        if(!find_symbol_info(callee_hash, name, kind))
            continue;

        protocol::CallHierarchyItem item;
        item.name = name;
        item.kind = to_lsp_symbol_kind(kind);
        item.uri = def_loc->uri;
        item.range = def_loc->range;
        item.selection_range = def_loc->range;
        item.data = protocol::LSPAny(static_cast<std::int64_t>(callee_hash));

        results.push_back({std::move(item), std::move(ranges)});
    }
    return results;
}

std::vector<protocol::TypeHierarchyItem> Indexer::find_supertypes(index::SymbolHash hash) {
    llvm::SmallVector<index::SymbolHash> base_hashes;
    collect_unique_targets(hash, RelationKind::Base, base_hashes);

    std::vector<protocol::TypeHierarchyItem> results;
    for(auto target_hash: base_hashes) {
        std::string name;
        SymbolKind kind;
        if(!find_symbol_info(target_hash, name, kind))
            continue;
        auto def_loc = find_definition_location(target_hash);
        if(!def_loc)
            continue;

        protocol::TypeHierarchyItem item;
        item.name = std::move(name);
        item.kind = to_lsp_symbol_kind(kind);
        item.uri = def_loc->uri;
        item.range = def_loc->range;
        item.selection_range = def_loc->range;
        item.data = protocol::LSPAny(static_cast<std::int64_t>(target_hash));
        results.push_back(std::move(item));
    }
    return results;
}

std::vector<protocol::TypeHierarchyItem> Indexer::find_subtypes(index::SymbolHash hash) {
    llvm::SmallVector<index::SymbolHash> derived_hashes;
    collect_unique_targets(hash, RelationKind::Derived, derived_hashes);

    std::vector<protocol::TypeHierarchyItem> results;
    for(auto target_hash: derived_hashes) {
        std::string name;
        SymbolKind kind;
        if(!find_symbol_info(target_hash, name, kind))
            continue;
        auto def_loc = find_definition_location(target_hash);
        if(!def_loc)
            continue;

        protocol::TypeHierarchyItem item;
        item.name = std::move(name);
        item.kind = to_lsp_symbol_kind(kind);
        item.uri = def_loc->uri;
        item.range = def_loc->range;
        item.selection_range = def_loc->range;
        item.data = protocol::LSPAny(static_cast<std::int64_t>(target_hash));
        results.push_back(std::move(item));
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

    for(auto& [hash, symbol]: project_index.symbols) {
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

    for(auto& [_, index]: open_file_indices) {
        if(results.size() >= max_results)
            break;
        for(auto& [hash, symbol]: index.symbols) {
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

// ── Indexer: static utilities ────────────────────────────────────────────

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

}  // namespace clice
