#include "server/index/query.h"

#include <algorithm>
#include <bit>
#include <string>
#include <variant>
#include <vector>

#include "index/tu_index.h"
#include "server/compiler/compiler.h"
#include "server/session/session.h"
#include "server/session/session_store.h"
#include "support/filesystem.h"

#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/uri.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Path.h"

namespace clice {

namespace lsp = kota::ipc::lsp;

void IndexQuery::visit_sessions(SessionVisitor visitor) const {
    sessions.for_each([&](std::uint32_t path_id, const Session& session) -> bool {
        // FIXME: when ast_dirty, consider awaiting recompilation
        // instead of silently falling back to MergedIndex.
        if(session.file_index && session.symbols && !session.ast_dirty) {
            return visitor(path_id, session);
        }
        return true;
    });
}

bool IndexQuery::is_path_open(std::uint32_t path_id) const {
    return sessions.find(path_id) != nullptr;
}

bool IndexQuery::find_symbol_info(index::SymbolHash hash,
                                  std::string& name,
                                  SymbolKind& kind) const {
    // Check open sessions first (has all symbols for unsaved buffers).
    bool found = false;
    visit_sessions([&](std::uint32_t, const Session& session) -> bool {
        if(!session.symbols)
            return true;
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

    // Check ProjectIndex (external symbols).
    auto it = workspace.project_index.symbols.find(hash);
    if(it != workspace.project_index.symbols.end()) {
        name = it->second.name;
        kind = it->second.kind;
        return true;
    }

    // Check per-file MergedIndex shards (TU-local + file-local symbols).
    // Each shard stores exactly the local symbols its occurrences reference,
    // so the symbol will be in the shard that produced the occurrence.
    for(auto& [path_id, shard]: workspace.merged_indices) {
        if(shard.find_symbol(hash, name, kind))
            return true;
    }
    return false;
}

IndexQuery::CursorHit IndexQuery::resolve_cursor(llvm::StringRef path,
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

    // Fallback to MergedIndex. Position -> offset uses the session text when
    // one exists (open but not yet compiled); for closed files the shard's
    // own stored content provides the mapping.
    auto path_id = workspace.path_pool.find(path);
    if(!path_id)
        return {};
    auto shard_it = workspace.merged_indices.find(*path_id);
    if(shard_it == workspace.merged_indices.end())
        return {};

    auto& merged_index = shard_it->second;
    auto ls = merged_index.line_starts();
    if(ls.empty())
        return {};
    lsp::LineMap map(merged_index.content(), ls);

    auto offset = session ? session->line_map().to_offset(position) : map.to_offset(position);
    if(!offset)
        return {};
    CursorHit hit;
    merged_index.lookup(*offset, [&](const index::Occurrence& o) {
        auto range = map.to_range(o.range.begin, o.range.end);
        if(range)
            hit = {o.target, *range};
        return false;
    });
    return hit;
}

std::vector<protocol::Location> IndexQuery::query_relations(llvm::StringRef path,
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
            if(is_path_open(file_id))
                continue;
            auto shard_it = workspace.merged_indices.find(file_id);
            if(shard_it == workspace.merged_indices.end())
                continue;
            auto uri = lsp::URI::from_file_path(workspace.path_pool.resolve(file_id));
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

    visit_sessions([&](std::uint32_t id, const Session& session) -> bool {
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

std::vector<protocol::Location> IndexQuery::query_symbol_targets(llvm::StringRef path,
                                                                 const protocol::Position& position,
                                                                 RelationKind kind,
                                                                 Session* session) {
    auto hit = resolve_cursor(path, position, session);
    if(hit.hash == 0)
        return {};

    llvm::SmallVector<index::SymbolHash> targets;
    collect_unique_targets(hit.hash, kind, targets);

    std::vector<protocol::Location> locations;
    for(auto target: targets) {
        if(auto info = resolve_symbol(target)) {
            locations.push_back({std::move(info->uri), info->range});
        }
    }
    return locations;
}

std::optional<SymbolInfo> IndexQuery::lookup_symbol(const std::string& uri,
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

std::optional<protocol::Location> IndexQuery::find_definition_location(index::SymbolHash hash) {
    std::optional<protocol::Location> session_result;
    visit_sessions([&](std::uint32_t id, const Session& session) -> bool {
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
        if(is_path_open(file_id))
            continue;
        auto shard_it = workspace.merged_indices.find(file_id);
        if(shard_it == workspace.merged_indices.end())
            continue;
        auto uri = lsp::URI::from_file_path(workspace.path_pool.resolve(file_id));
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
    IndexQuery::resolve_hierarchy_item(const std::string& uri,
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

void IndexQuery::collect_grouped_relations(
    index::SymbolHash hash,
    RelationKind kind,
    llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>>& target_ranges) {
    auto sym_it = workspace.project_index.symbols.find(hash);
    if(sym_it != workspace.project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(is_path_open(file_id))
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
    visit_sessions([&](std::uint32_t, const Session& session) -> bool {
        auto map = session.line_map();
        session.file_index->lookup(hash, kind, [&](const index::Relation& r) {
            if(auto range = map.to_range(r.range.begin, r.range.end))
                target_ranges[r.target_symbol].push_back(*range);
            return true;
        });
        return true;
    });
}

void IndexQuery::collect_unique_targets(index::SymbolHash hash,
                                        RelationKind kind,
                                        llvm::SmallVectorImpl<index::SymbolHash>& targets) {
    llvm::DenseSet<index::SymbolHash> seen;
    auto sym_it = workspace.project_index.symbols.find(hash);
    if(sym_it != workspace.project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(is_path_open(file_id))
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
    visit_sessions([&](std::uint32_t, const Session& session) -> bool {
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
std::optional<SymbolInfo> IndexQuery::resolve_symbol(index::SymbolHash hash) {
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

std::optional<IndexQuery::DefinitionText> IndexQuery::get_definition_text(index::SymbolHash hash) {
    std::optional<DefinitionText> session_result;
    visit_sessions([&](std::uint32_t id, const Session& session) -> bool {
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
        if(is_path_open(file_id))
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
                .file = workspace.path_pool.resolve(file_id).str(),
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

std::vector<IndexQuery::ReferenceWithContext> IndexQuery::collect_references(index::SymbolHash hash,
                                                                             RelationKind kind) {
    std::vector<ReferenceWithContext> results;

    auto sym_it = workspace.project_index.symbols.find(hash);
    if(sym_it != workspace.project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(is_path_open(file_id))
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
            auto file_path = workspace.path_pool.resolve(file_id);

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

    visit_sessions([&](std::uint32_t id, const Session& session) -> bool {
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
    IndexQuery::find_incoming_calls(index::SymbolHash hash) {
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
    IndexQuery::find_outgoing_calls(index::SymbolHash hash) {
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

std::vector<protocol::TypeHierarchyItem> IndexQuery::find_supertypes(index::SymbolHash hash) {
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

std::vector<protocol::TypeHierarchyItem> IndexQuery::find_subtypes(index::SymbolHash hash) {
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

std::vector<protocol::SymbolInformation> IndexQuery::search_symbols(llvm::StringRef query,
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

    visit_sessions([&](std::uint32_t, const Session& session) -> bool {
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

std::vector<ResolvedSymbol> IndexQuery::locate_symbols(const agentic::ReadSymbolParams& loc) {
    if(loc.symbol_id.has_value() && *loc.symbol_id != 0) {
        auto hash = static_cast<index::SymbolHash>(*loc.symbol_id);
        std::string name;
        SymbolKind kind;
        if(!find_symbol_info(hash, name, kind))
            return {};
        auto def_loc = find_definition_location(hash);
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
            auto def_loc = find_definition_location(hash);
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
        visit_sessions([&](std::uint32_t, const Session& session) -> bool {
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
            with_session(server_id, [&](const Session& session) {
                auto map = session.line_map();
                for(auto& [hash, rels]: session.file_index->relations) {
                    for(auto& rel: rels) {
                        if(rel.kind.value() != RelationKind::Definition)
                            continue;
                        auto start = map.to_position(rel.range.begin);
                        if(start && start->line == target_line) {
                            std::string name;
                            SymbolKind kind;
                            if(!find_symbol_info(hash, name, kind))
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

        auto path_id = workspace.path_pool.find(path_str);
        if(!path_id)
            return {};

        auto shard_it = workspace.merged_indices.find(*path_id);
        if(shard_it == workspace.merged_indices.end())
            return {};

        auto& merged_index = shard_it->second;
        auto ls = merged_index.line_starts();
        if(ls.empty())
            return {};
        lsp::LineMap map(merged_index.content(), ls);

        for(auto& [hash, symbol]: workspace.project_index.symbols) {
            if(!symbol.reference_files.contains(*path_id))
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

protocol::SymbolKind IndexQuery::to_lsp_symbol_kind(SymbolKind kind) {
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

protocol::CallHierarchyItem IndexQuery::build_call_hierarchy_item(const SymbolInfo& info) {
    protocol::CallHierarchyItem item;
    item.name = info.name;
    item.kind = to_lsp_symbol_kind(info.kind);
    item.uri = info.uri;
    item.range = info.range;
    item.selection_range = info.range;
    item.data = protocol::LSPAny(static_cast<std::int64_t>(info.hash));
    return item;
}

protocol::TypeHierarchyItem IndexQuery::build_type_hierarchy_item(const SymbolInfo& info) {
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
