#include "server/indexer.h"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

#include "eventide/ipc/json_codec.h"
#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/protocol.h"
#include "eventide/ipc/lsp/uri.h"
#include "eventide/serde/json/json.h"
#include "index/tu_index.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace clice {

namespace lsp = eventide::ipc::lsp;
using serde_raw = et::serde::RawValue;

template <typename T>
static serde_raw to_raw(const T& value) {
    auto json = et::serde::json::to_json<et::ipc::lsp_config>(value);
    return serde_raw{json ? std::move(*json) : "null"};
}

/// Look up the first occurrence containing `offset` in a sorted occurrence list.
/// Uses lower_bound on range.end, then scans forward through overlapping
/// occurrences (e.g. nested templates, macro expansions) to find the tightest
/// (innermost) match.  Returns nullptr if no occurrence contains the offset.
static const index::Occurrence* lookup_occurrence(const std::vector<index::Occurrence>& occs,
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

// =========================================================================
// Data management
// =========================================================================

void Indexer::merge(const void* tu_index_data, std::size_t size) {
    auto tu_index = index::TUIndex::from(tu_index_data);

    auto file_ids_map = project_index.merge(tu_index);
    auto main_tu_path_id = static_cast<std::uint32_t>(tu_index.graph.paths.size() - 1);

    auto merge_file_index = [&](std::uint32_t tu_path_id, index::FileIndex& file_idx) {
        auto global_path_id = file_ids_map[tu_path_id];
        auto& merged = merged_indices[global_path_id];

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
            merged.merge(global_path_id,
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
            merged.merge(global_path_id, include_id, file_idx, header_content);
        }
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
    for(auto& [path_id, merged]: merged_indices) {
        if(!merged.need_rewrite())
            continue;
        auto shard_path = path::join(shards_dir, std::to_string(path_id) + ".idx");
        std::error_code write_ec;
        llvm::raw_fd_ostream os(shard_path, write_ec);
        if(!write_ec) {
            merged.serialize(os);
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

        merged_indices[path_id] = index::MergedIndex::load(it->path());
    }

    if(!merged_indices.empty()) {
        LOG_INFO("Loaded {} MergedIndex shards", merged_indices.size());
    }
}

bool Indexer::need_update(llvm::StringRef file_path) {
    auto cache_it = project_index.path_pool.find(file_path);
    if(cache_it == project_index.path_pool.cache.end())
        return true;  // Never indexed.

    auto proj_path_id = cache_it->second;
    auto merged_it = merged_indices.find(proj_path_id);
    if(merged_it == merged_indices.end())
        return true;  // No shard.

    llvm::SmallVector<llvm::StringRef> path_mapping;
    for(auto& p: project_index.path_pool.paths) {
        path_mapping.push_back(p);
    }
    return merged_it->second.need_update(path_mapping);
}

// =========================================================================
// Open file index management
// =========================================================================

void Indexer::set_open_file(std::uint32_t server_path_id,
                            llvm::StringRef file_path,
                            OpenFileIndex ofi) {
    open_file_indices[server_path_id] = std::move(ofi);
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

// =========================================================================
// Symbol queries
// =========================================================================

bool Indexer::find_symbol_info(index::SymbolHash hash,
                               std::string& name,
                               SymbolKind& kind) const {
    // Open file indices may have symbols not yet in ProjectIndex.
    for(auto& [_, ofi]: open_file_indices) {
        auto it = ofi.symbols.find(hash);
        if(it != ofi.symbols.end()) {
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

et::serde::RawValue Indexer::query_relations(llvm::StringRef path,
                                             std::uint32_t server_path_id,
                                             const protocol::Position& position,
                                             RelationKind kind,
                                             const std::string* doc_text) {
    // Step 1: Find occurrence at the cursor position.
    index::SymbolHash symbol_hash = 0;

    auto ofi_it = open_file_indices.find(server_path_id);
    if(ofi_it != open_file_indices.end()) {
        lsp::PositionMapper mapper(ofi_it->second.content, lsp::PositionEncoding::UTF16);
        auto offset_opt = mapper.to_offset(position);
        if(!offset_opt)
            return serde_raw{"null"};

        if(auto* occ = lookup_occurrence(ofi_it->second.file_index.occurrences, *offset_opt)) {
            symbol_hash = occ->target;
        }
    } else {
        if(!doc_text)
            return serde_raw{"null"};

        lsp::PositionMapper mapper(*doc_text, lsp::PositionEncoding::UTF16);
        auto offset_opt = mapper.to_offset(position);
        if(!offset_opt)
            return serde_raw{"null"};

        auto proj_cache_it = project_index.path_pool.find(path);
        if(proj_cache_it == project_index.path_pool.cache.end())
            return serde_raw{"null"};

        auto merged_it = merged_indices.find(proj_cache_it->second);
        if(merged_it == merged_indices.end())
            return serde_raw{"null"};

        merged_it->second.lookup(*offset_opt, [&](const index::Occurrence& o) {
            symbol_hash = o.target;
            return false;
        });
    }

    if(symbol_hash == 0)
        return serde_raw{"null"};

    // Step 2: Collect relations from all sources.
    std::vector<protocol::Location> locations;

    // 2a: From ProjectIndex reference files (MergedIndex shards).
    auto sym_it = project_index.symbols.find(symbol_hash);
    if(sym_it != project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(open_proj_path_ids.contains(file_id))
                continue;

            auto file_merged_it = merged_indices.find(file_id);
            if(file_merged_it == merged_indices.end())
                continue;

            auto file_path = project_index.path_pool.path(file_id);
            auto file_uri = lsp::URI::from_file_path(file_path);
            if(!file_uri)
                continue;

            auto file_content = file_merged_it->second.content();
            if(file_content.empty())
                continue;

            lsp::PositionMapper file_mapper(file_content, lsp::PositionEncoding::UTF16);

            file_merged_it->second.lookup(symbol_hash, kind, [&](const index::Relation& r) {
                auto start = file_mapper.to_position(r.range.begin);
                auto end = file_mapper.to_position(r.range.end);
                if(start && end) {
                    protocol::Location loc;
                    loc.uri = file_uri->str();
                    loc.range = protocol::Range{*start, *end};
                    locations.push_back(std::move(loc));
                }
                return true;
            });
        }
    }

    // 2b: From all open file indices.
    for(auto& [ofi_server_id, ofi]: open_file_indices) {
        auto rel_it = ofi.file_index.relations.find(symbol_hash);
        if(rel_it == ofi.file_index.relations.end())
            continue;

        auto ofi_path = std::string(path_pool.resolve(ofi_server_id));
        auto ofi_uri = lsp::URI::from_file_path(ofi_path);
        if(!ofi_uri)
            continue;

        lsp::PositionMapper ofi_mapper(ofi.content, lsp::PositionEncoding::UTF16);

        for(auto& relation: rel_it->second) {
            if(relation.kind & kind) {
                auto start = ofi_mapper.to_position(relation.range.begin);
                auto end = ofi_mapper.to_position(relation.range.end);
                if(start && end) {
                    protocol::Location loc;
                    loc.uri = ofi_uri->str();
                    loc.range = protocol::Range{*start, *end};
                    locations.push_back(std::move(loc));
                }
            }
        }
    }

    if(locations.empty())
        return serde_raw{"null"};

    return to_raw(locations);
}

std::optional<SymbolInfo> Indexer::lookup_symbol(const std::string& uri,
                                                 llvm::StringRef path,
                                                 std::uint32_t server_path_id,
                                                 const protocol::Position& position,
                                                 const std::string* doc_text) {
    index::SymbolHash symbol_hash = 0;
    index::Range occ_range{};
    lsp::PositionMapper* mapper_ptr = nullptr;

    std::optional<lsp::PositionMapper> ofi_mapper;
    auto ofi_it = open_file_indices.find(server_path_id);
    if(ofi_it != open_file_indices.end()) {
        ofi_mapper.emplace(ofi_it->second.content, lsp::PositionEncoding::UTF16);
        mapper_ptr = &*ofi_mapper;
        auto offset_opt = ofi_mapper->to_offset(position);
        if(!offset_opt)
            return std::nullopt;

        if(auto* occ = lookup_occurrence(ofi_it->second.file_index.occurrences, *offset_opt)) {
            symbol_hash = occ->target;
            occ_range = occ->range;
        }
    } else {
        if(!doc_text)
            return std::nullopt;

        ofi_mapper.emplace(*doc_text, lsp::PositionEncoding::UTF16);
        mapper_ptr = &*ofi_mapper;
        auto offset_opt = ofi_mapper->to_offset(position);
        if(!offset_opt)
            return std::nullopt;

        auto proj_cache_it = project_index.path_pool.find(path);
        if(proj_cache_it == project_index.path_pool.cache.end())
            return std::nullopt;

        auto merged_it = merged_indices.find(proj_cache_it->second);
        if(merged_it == merged_indices.end())
            return std::nullopt;

        merged_it->second.lookup(*offset_opt, [&](const index::Occurrence& o) {
            symbol_hash = o.target;
            occ_range = o.range;
            return false;
        });
    }

    if(symbol_hash == 0)
        return std::nullopt;

    std::string name;
    SymbolKind sym_kind;
    if(!find_symbol_info(symbol_hash, name, sym_kind))
        return std::nullopt;

    auto start = mapper_ptr->to_position(occ_range.begin);
    auto end = mapper_ptr->to_position(occ_range.end);
    if(!start || !end)
        return std::nullopt;

    SymbolInfo info;
    info.hash = symbol_hash;
    info.name = std::move(name);
    info.kind = sym_kind;
    info.uri = uri;
    info.range = protocol::Range{*start, *end};
    return info;
}

std::optional<protocol::Location> Indexer::find_definition_location(index::SymbolHash hash) {
    // Check open file indices first (may have the most up-to-date definition).
    for(auto& [ofi_server_id, ofi]: open_file_indices) {
        auto rel_it = ofi.file_index.relations.find(hash);
        if(rel_it == ofi.file_index.relations.end())
            continue;

        auto ofi_path = std::string(path_pool.resolve(ofi_server_id));
        auto ofi_uri = lsp::URI::from_file_path(ofi_path);
        if(!ofi_uri)
            continue;

        lsp::PositionMapper mapper(ofi.content, lsp::PositionEncoding::UTF16);
        for(auto& relation: rel_it->second) {
            if(relation.kind.is_one_of(RelationKind::Definition)) {
                auto start = mapper.to_position(relation.range.begin);
                auto end = mapper.to_position(relation.range.end);
                if(start && end) {
                    protocol::Location loc;
                    loc.uri = ofi_uri->str();
                    loc.range = protocol::Range{*start, *end};
                    return loc;
                }
            }
        }
    }

    // Fall back to ProjectIndex reference files (MergedIndex shards).
    auto sym_it = project_index.symbols.find(hash);
    if(sym_it == project_index.symbols.end())
        return std::nullopt;

    for(auto file_id: sym_it->second.reference_files) {
        if(open_proj_path_ids.contains(file_id))
            continue;

        auto file_merged_it = merged_indices.find(file_id);
        if(file_merged_it == merged_indices.end())
            continue;

        auto file_path = project_index.path_pool.path(file_id);
        auto file_uri = lsp::URI::from_file_path(file_path);
        if(!file_uri)
            continue;

        auto file_content = file_merged_it->second.content();
        if(file_content.empty())
            continue;
        lsp::PositionMapper file_mapper(file_content, lsp::PositionEncoding::UTF16);

        std::optional<protocol::Location> result;
        file_merged_it->second.lookup(hash,
                                      RelationKind::Definition,
                                      [&](const index::Relation& r) {
                                          auto start = file_mapper.to_position(r.range.begin);
                                          auto end = file_mapper.to_position(r.range.end);
                                          if(start && end) {
                                              protocol::Location loc;
                                              loc.uri = file_uri->str();
                                              loc.range = protocol::Range{*start, *end};
                                              result = std::move(loc);
                                              return false;
                                          }
                                          return true;
                                      });

        if(result)
            return result;
    }

    return std::nullopt;
}

std::optional<SymbolInfo> Indexer::resolve_hierarchy_item(
    const std::string& uri,
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
                SymbolInfo info;
                info.hash = hash;
                info.name = std::move(name);
                info.kind = kind;
                info.uri = uri;
                info.range = range;
                return info;
            }
        }
    }

    return lookup_symbol(uri, path, server_path_id, range.start, doc_text);
}

// =========================================================================
// Hierarchy & workspace queries
// =========================================================================

/// Helper to collect relations of a given kind for a symbol, grouping by target.
/// Iterates both MergedIndex shards and open file indices.
static void collect_relations(
    const index::ProjectIndex& project_index,
    const llvm::DenseMap<std::uint32_t, index::MergedIndex>& merged_indices,
    const llvm::DenseMap<std::uint32_t, OpenFileIndex>& open_file_indices,
    const llvm::DenseSet<std::uint32_t>& open_proj_path_ids,
    const PathPool& path_pool,
    index::SymbolHash hash,
    RelationKind kind,
    llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>>& target_ranges) {
    auto sym_it = project_index.symbols.find(hash);
    if(sym_it != project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(open_proj_path_ids.contains(file_id))
                continue;

            auto file_merged_it = merged_indices.find(file_id);
            if(file_merged_it == merged_indices.end())
                continue;

            auto file_content = file_merged_it->second.content();
            if(file_content.empty())
                continue;

            lsp::PositionMapper file_mapper(file_content, lsp::PositionEncoding::UTF16);

            file_merged_it->second.lookup(hash, kind, [&](const index::Relation& r) {
                auto start = file_mapper.to_position(r.range.begin);
                auto end = file_mapper.to_position(r.range.end);
                if(start && end) {
                    target_ranges[r.target_symbol].push_back(protocol::Range{*start, *end});
                }
                return true;
            });
        }
    }

    for(auto& [ofi_id, ofi]: open_file_indices) {
        auto rel_it = ofi.file_index.relations.find(hash);
        if(rel_it == ofi.file_index.relations.end())
            continue;
        lsp::PositionMapper ofi_mapper(ofi.content, lsp::PositionEncoding::UTF16);
        for(auto& r: rel_it->second) {
            if(r.kind == kind) {
                auto start = ofi_mapper.to_position(r.range.begin);
                auto end = ofi_mapper.to_position(r.range.end);
                if(start && end) {
                    target_ranges[r.target_symbol].push_back(protocol::Range{*start, *end});
                }
            }
        }
    }
}

/// Helper to collect type hierarchy relations (Base/Derived).
static void collect_type_relations(
    const index::ProjectIndex& project_index,
    const llvm::DenseMap<std::uint32_t, index::MergedIndex>& merged_indices,
    const llvm::DenseMap<std::uint32_t, OpenFileIndex>& open_file_indices,
    const llvm::DenseSet<std::uint32_t>& open_proj_path_ids,
    index::SymbolHash hash,
    RelationKind kind,
    llvm::SmallVectorImpl<index::SymbolHash>& targets) {
    llvm::DenseSet<index::SymbolHash> seen;

    auto sym_it = project_index.symbols.find(hash);
    if(sym_it != project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(open_proj_path_ids.contains(file_id))
                continue;

            auto file_merged_it = merged_indices.find(file_id);
            if(file_merged_it == merged_indices.end())
                continue;

            file_merged_it->second.lookup(hash, kind, [&](const index::Relation& r) {
                if(seen.insert(r.target_symbol).second) {
                    targets.push_back(r.target_symbol);
                }
                return true;
            });
        }
    }

    for(auto& [ofi_id, ofi]: open_file_indices) {
        auto rel_it = ofi.file_index.relations.find(hash);
        if(rel_it == ofi.file_index.relations.end())
            continue;
        for(auto& r: rel_it->second) {
            if(r.kind == kind) {
                if(seen.insert(r.target_symbol).second) {
                    targets.push_back(r.target_symbol);
                }
            }
        }
    }
}

et::serde::RawValue Indexer::find_incoming_calls(index::SymbolHash hash) {
    llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>> caller_ranges;
    collect_relations(
        project_index, merged_indices, open_file_indices, open_proj_path_ids, path_pool,
        hash, RelationKind::Caller, caller_ranges);

    std::vector<protocol::CallHierarchyIncomingCall> results;
    for(auto& [caller_hash, ranges]: caller_ranges) {
        auto def_loc = find_definition_location(caller_hash);
        if(!def_loc)
            continue;

        std::string caller_name;
        SymbolKind caller_kind;
        if(!find_symbol_info(caller_hash, caller_name, caller_kind))
            continue;

        protocol::CallHierarchyItem caller_item;
        caller_item.name = caller_name;
        caller_item.kind = to_lsp_symbol_kind(caller_kind);
        caller_item.uri = def_loc->uri;
        caller_item.range = def_loc->range;
        caller_item.selection_range = def_loc->range;
        caller_item.data = protocol::LSPAny(static_cast<std::int64_t>(caller_hash));

        protocol::CallHierarchyIncomingCall call;
        call.from = std::move(caller_item);
        call.from_ranges = std::move(ranges);
        results.push_back(std::move(call));
    }

    if(results.empty())
        return serde_raw{"null"};
    return to_raw(results);
}

et::serde::RawValue Indexer::find_outgoing_calls(index::SymbolHash hash) {
    llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>> callee_ranges;
    collect_relations(
        project_index, merged_indices, open_file_indices, open_proj_path_ids, path_pool,
        hash, RelationKind::Callee, callee_ranges);

    std::vector<protocol::CallHierarchyOutgoingCall> results;
    for(auto& [callee_hash, ranges]: callee_ranges) {
        auto def_loc = find_definition_location(callee_hash);
        if(!def_loc)
            continue;

        std::string callee_name;
        SymbolKind callee_kind;
        if(!find_symbol_info(callee_hash, callee_name, callee_kind))
            continue;

        protocol::CallHierarchyItem callee_item;
        callee_item.name = callee_name;
        callee_item.kind = to_lsp_symbol_kind(callee_kind);
        callee_item.uri = def_loc->uri;
        callee_item.range = def_loc->range;
        callee_item.selection_range = def_loc->range;
        callee_item.data = protocol::LSPAny(static_cast<std::int64_t>(callee_hash));

        protocol::CallHierarchyOutgoingCall call;
        call.to = std::move(callee_item);
        call.from_ranges = std::move(ranges);
        results.push_back(std::move(call));
    }

    if(results.empty())
        return serde_raw{"null"};
    return to_raw(results);
}

et::serde::RawValue Indexer::find_supertypes(index::SymbolHash hash) {
    llvm::SmallVector<index::SymbolHash> base_hashes;
    collect_type_relations(
        project_index, merged_indices, open_file_indices, open_proj_path_ids,
        hash, RelationKind::Base, base_hashes);

    std::vector<protocol::TypeHierarchyItem> results;
    for(auto base_hash: base_hashes) {
        std::string base_name;
        SymbolKind base_kind;
        if(!find_symbol_info(base_hash, base_name, base_kind))
            continue;

        auto def_loc = find_definition_location(base_hash);
        if(!def_loc)
            continue;

        protocol::TypeHierarchyItem item;
        item.name = std::move(base_name);
        item.kind = to_lsp_symbol_kind(base_kind);
        item.uri = def_loc->uri;
        item.range = def_loc->range;
        item.selection_range = def_loc->range;
        item.data = protocol::LSPAny(static_cast<std::int64_t>(base_hash));
        results.push_back(std::move(item));
    }

    if(results.empty())
        return serde_raw{"null"};
    return to_raw(results);
}

et::serde::RawValue Indexer::find_subtypes(index::SymbolHash hash) {
    llvm::SmallVector<index::SymbolHash> derived_hashes;
    collect_type_relations(
        project_index, merged_indices, open_file_indices, open_proj_path_ids,
        hash, RelationKind::Derived, derived_hashes);

    std::vector<protocol::TypeHierarchyItem> results;
    for(auto derived_hash: derived_hashes) {
        std::string derived_name;
        SymbolKind derived_kind;
        if(!find_symbol_info(derived_hash, derived_name, derived_kind))
            continue;

        auto def_loc = find_definition_location(derived_hash);
        if(!def_loc)
            continue;

        protocol::TypeHierarchyItem item;
        item.name = std::move(derived_name);
        item.kind = to_lsp_symbol_kind(derived_kind);
        item.uri = def_loc->uri;
        item.range = def_loc->range;
        item.selection_range = def_loc->range;
        item.data = protocol::LSPAny(static_cast<std::int64_t>(derived_hash));
        results.push_back(std::move(item));
    }

    if(results.empty())
        return serde_raw{"null"};
    return to_raw(results);
}

et::serde::RawValue Indexer::search_symbols(llvm::StringRef query, std::size_t max_results) {
    std::string query_lower = query.lower();

    auto is_indexable_kind = [](SymbolKind sk) {
        return sk == SymbolKind::Namespace || sk == SymbolKind::Class ||
               sk == SymbolKind::Struct || sk == SymbolKind::Union || sk == SymbolKind::Enum ||
               sk == SymbolKind::Type || sk == SymbolKind::Field ||
               sk == SymbolKind::EnumMember || sk == SymbolKind::Function ||
               sk == SymbolKind::Method || sk == SymbolKind::Variable ||
               sk == SymbolKind::Parameter || sk == SymbolKind::Macro ||
               sk == SymbolKind::Concept || sk == SymbolKind::Module ||
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

    // Also search open file indices for symbols not in ProjectIndex.
    for(auto& [ofi_server_id, ofi]: open_file_indices) {
        if(results.size() >= max_results)
            break;
        for(auto& [hash, symbol]: ofi.symbols) {
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

    if(results.empty())
        return serde_raw{"null"};
    return to_raw(results);
}

// =========================================================================
// Static utilities
// =========================================================================

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
