#include "server/service/query.h"

#include <algorithm>
#include <bit>
#include <format>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "feature/feature.h"
#include "index/tu_index.h"
#include "server/compiler/compiler.h"
#include "server/compiler/indexer.h"
#include "server/protocol/position.h"
#include "server/state/session.h"
#include "server/state/session_store.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"

#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/uri.h"
#include "kota/meta/enum.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice {

namespace lsp = kota::ipc::lsp;

void IndexQuery::visit_sessions(SessionVisitor visitor) const {
    if(options.disk_only) {
        return;
    }
    sessions.for_each([&](std::uint32_t path_id, const Session& session) -> bool {
        // Freshness contract, clause 3: a dirty session's file index may
        // describe a buffer that no longer exists — skip it.
        if(session.index.loaded() && !session.ast_dirty) {
            return visitor(path_id, session);
        }
        return true;
    });
}

bool IndexQuery::is_path_open(std::uint32_t path_id) const {
    return sessions.find(path_id) != nullptr;
}

std::shared_ptr<index::TUIndex> IndexQuery::overlay_of(const Session& session) const {
    if(!session.pch_key) {
        return nullptr;
    }
    // Returned by value: consumers run synchronously, but a reference into
    // the map value would not survive a rehash.
    return workspace.preamble_state(*session.pch_key);
}

void IndexQuery::visit_overlays(llvm::function_ref<bool(const index::TUIndex&)> visitor) const {
    if(options.disk_only) {
        return;
    }
    // Sessions with identical preambles share one blob; visit it once.
    llvm::StringSet<> seen;
    sessions.for_each([&](std::uint32_t, const Session& session) -> bool {
        if(!session.pch_key || !seen.insert(*session.pch_key).second) {
            return true;
        }
        auto state = overlay_of(session);
        return state ? visitor(*state) : true;
    });
}

void IndexQuery::visit_preambles(
    llvm::function_ref<bool(std::uint32_t, const Session&, const index::TUIndex&)> visitor) const {
    if(options.disk_only) {
        return;
    }
    sessions.for_each([&](std::uint32_t path_id, const Session& session) -> bool {
        auto state = overlay_of(session);
        if(!state || !serves_preamble(session, *state)) {
            return true;
        }
        return visitor(path_id, session, *state);
    });
}

bool IndexQuery::serves_preamble(const Session& session, const index::TUIndex& state) const {
    // The preamble entry's rows are buffer offsets of the file that built
    // the blob: serve them only for that very file (identical preambles
    // share a PCH, but macro USRs embed the source path) and only while
    // the buffer still starts with the exact preamble text the blob was
    // built from. The prefix comparison validates the described region
    // directly — body edits never move preamble rows — so no dirty-flag
    // gating is needed on top. The blob stores clang's native path
    // (backslashes on Windows) while the pool normalizes separators, so
    // compare through the pool's lookup, not raw strings.
    return workspace.path_pool.find(state.path(state.path_count() - 1)) == session.path_id &&
           state.matches_prefix(session.text);
}

bool IndexQuery::should_serve_overlay_file(llvm::StringRef path) const {
    // An open file serves its own buffer-true rows (its session, plus the
    // is_path_open shard skip); overlay rows for it were computed from the
    // disk snapshot and would map onto the edited buffer at wrong lines —
    // and dedup cannot collapse them, since the positions differ.
    // Freshness contract, clause 2, same as shards: a file whose own
    // content changed on disk has its rows suppressed until an up-to-date
    // view lands — the blob snapshot describes text that no longer exists.
    if(auto path_id = workspace.path_pool.find(path)) {
        if(is_path_open(*path_id) || skip_stale_contribution(*path_id)) {
            return false;
        }
    }
    return !workspace.is_synthesized_artifact(path);
}

/// A header entry of an overlay envelope, as lookup callbacks consume it.
/// Views borrow the envelope; keep the overlay alive while using them.
struct OverlayFile {
    llvm::StringRef path;

    /// Empty for pure-ASCII content, which blobs do not store.
    llvm::StringRef content;

    std::uint32_t content_size = 0;

    std::span<const std::uint32_t> line_starts;
};

/// Iterate relations of `symbol` matching `kind` across an overlay
/// envelope's header entries (every section except the source file's
/// own, which has its own serving gate). This is the only query shape
/// overlays serve: hash-anchored answering; discovery inputs (by name,
/// by path and line) are the disk index's job.
static void
    overlay_lookup(const index::TUIndex& state,
                   index::SymbolHash symbol,
                   RelationKind kind,
                   llvm::function_ref<bool(const OverlayFile&, const index::Relation&)> callback) {
    auto main_id = state.path_count() - 1;
    for(std::uint32_t i = 0; i < state.section_count(); i += 1) {
        auto path_id = state.section_path(i);
        if(path_id == main_id) {
            continue;
        }
        auto& shard = state.shard_of(path_id);
        OverlayFile file{
            .path = state.path(path_id),
            .content = shard.content(),
            .content_size = shard.content_size(),
            .line_starts = shard.line_starts(),
        };
        bool stopped = false;
        shard.lookup(symbol, kind, [&](const index::Relation& relation) {
            if(!callback(file, relation)) {
                stopped = true;
                return false;
            }
            return true;
        });
        if(stopped) {
            return;
        }
    }
}

/// The source file's preamble-region rows of an overlay envelope (buffer
/// offsets below the preamble bound).
const static index::Shard& preamble_rows(const index::TUIndex& state) {
    return state.shard_of(state.path_count() - 1);
}

/// Cross-source dedup: a row present in both a disk shard and a PCH
/// overlay (or in two overlays sharing a preamble) comes out identical.
static void dedup_locations(std::vector<protocol::Location>& locations) {
    auto key = [](const protocol::Location& location) {
        return std::tie(location.uri,
                        location.range.start.line,
                        location.range.start.character,
                        location.range.end.line,
                        location.range.end.character);
    };
    std::ranges::sort(locations,
                      [&](const auto& lhs, const auto& rhs) { return key(lhs) < key(rhs); });
    auto dup = std::ranges::unique(locations, [&](const auto& lhs, const auto& rhs) {
        return key(lhs) == key(rhs);
    });
    locations.erase(dup.begin(), dup.end());
}

bool IndexQuery::skip_shard(std::uint32_t path_id) const {
    return (!options.disk_only && is_path_open(path_id)) || skip_stale_contribution(path_id);
}

bool IndexQuery::skip_stale_contribution(std::uint32_t path_id) const {
    // With background indexing disabled nothing ever catches up: serving
    // the last-known rows beats a permanent hole.
    if(!workspace.config.project.enable_indexing.value) {
        return false;
    }
    return indexer.pending_reason(path_id) == ReindexReason::ContentChanged;
}

bool IndexQuery::find_symbol_info(index::SymbolHash hash,
                                  std::string& name,
                                  SymbolKind& kind) const {
    // Check open sessions first (has all symbols for unsaved buffers).
    bool found = false;
    visit_sessions([&](std::uint32_t, const Session& session) -> bool {
        if(auto identity = session.index.find_symbol(hash)) {
            name = std::string(identity->name);
            kind = identity->kind;
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

    // Check PCH overlays: a symbol that exists only under an open buffer's
    // context (or in headers no disk TU has been indexed with) is in no
    // disk table.
    visit_overlays([&](const index::TUIndex& state) {
        if(auto identity = state.find_symbol(hash)) {
            name = std::string(identity->name);
            kind = identity->kind;
            found = true;
        }
        return !found;
    });
    if(found)
        return true;

    // Check per-file Shard blobs (TU-local + file-local symbols).
    // Each shard stores exactly the local symbols its occurrences reference,
    // so the symbol will be in the shard that produced the occurrence.
    for(auto& [path_id, shard]: workspace.shards) {
        if(shard.find_symbol(hash, name, kind))
            return true;
    }
    return false;
}

IndexQuery::CursorHit IndexQuery::resolve_cursor(llvm::StringRef path,
                                                 const protocol::Position& position,
                                                 Session* session) {
    // Freshness contract, clause 1: callers awaited the session's compile,
    // and the file index settles together with it. An open document
    // resolves against that index or not at all — its shard describes a
    // disk snapshot, and mapping live buffer offsets onto it is exactly
    // the mixed-view lookup the contract exists to prevent (the
    // cross-file visit already skips shards of open files for the same
    // reason). A dirty-after-await session (failed or superseded compile)
    // or an index-less one therefore reports no hit.
    if(session && (!session->index.loaded() || session->ast_dirty)) {
        return {};
    }
    if(session && session->index.loaded() && !session->ast_dirty) {
        auto map = session->line_map();
        auto offset = map.to_offset(position);
        if(!offset)
            return {};
        CursorHit hit;
        session->file_rows().lookup(*offset, [&](const index::Occurrence& occ) {
            auto range = map.to_range(occ.range.begin, occ.range.end);
            if(range) {
                hit = {occ.target, *range};
                return false;
            }
            return true;
        });
        // The preamble region is compiled into the PCH and invisible to
        // the per-edit index; its occurrences (macro definitions and
        // references before the bound) live in the PCH's overlay, in the
        // same buffer coordinates — served only under the main-entry gate
        // (preamble drift, shared-PCH identity).
        auto overlay = hit.hash == 0 ? overlay_of(*session) : nullptr;
        if(overlay && serves_preamble(*session, *overlay)) {
            preamble_rows(*overlay).lookup(*offset, [&](const index::Occurrence& occ) {
                auto range = map.to_range(occ.range.begin, occ.range.end);
                if(range) {
                    hit = {occ.target, *range};
                    return false;
                }
                return true;
            });
        }
        return hit;
    }

    // Fallback to the disk shard. Position -> offset uses the session text when
    // one exists (open but not yet compiled); for closed files the shard's
    // own stored content provides the mapping.
    auto path_id = workspace.path_pool.find(path);
    if(!path_id)
        return {};
    // A content-changed pending file's rows describe stale text: a cursor
    // resolved against them would name the wrong symbol.
    if(skip_stale_contribution(*path_id))
        return {};
    auto shard_it = workspace.shards.find(*path_id);
    if(shard_it == workspace.shards.end())
        return {};

    auto& merged_index = shard_it->second;
    IndexedLineMap map(merged_index.content(),
                       merged_index.content_size(),
                       merged_index.line_starts());

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
    ScopedTimer timer;
    auto hit = resolve_cursor(path, position, session);
    if(hit.hash == 0) {
        // Misses (whitespace, comments, unindexed positions) are normal
        // inputs; their latency belongs in the series like any hit's.
        LOG_PERF("index_query",
                 "kind=relations rel={} path={} results=0 elapsed_ms={:.2f}",
                 kota::meta::enum_name(static_cast<RelationKind::Kind>(kind), "Invalid"),
                 path,
                 timer.ms_f());
        return {};
    }

    std::vector<protocol::Location> locations;

    auto sym_it = workspace.project_index.symbols.find(hit.hash);
    if(sym_it != workspace.project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(skip_shard(file_id))
                continue;
            auto shard_it = workspace.shards.find(file_id);
            if(shard_it == workspace.shards.end())
                continue;
            auto uri = lsp::URI::from_file_path(workspace.path_pool.resolve(file_id));
            if(!uri)
                continue;
            auto& merged_index = shard_it->second;
            IndexedLineMap map(merged_index.content(),
                               merged_index.content_size(),
                               merged_index.line_starts());
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
        session.file_rows().lookup(hit.hash, kind, [&](const index::Relation& r) {
            if(auto range = map.to_range(r.range.begin, r.range.end))
                locations.push_back({uri->str(), *range});
            return true;
        });
        return true;
    });

    // PCH overlays: header rows under each open buffer's live context.
    // Rows a disk shard also holds come out identical and collapse in the
    // dedup below.
    visit_overlays([&](const index::TUIndex& state) {
        overlay_lookup(state,
                       hit.hash,
                       kind,
                       [&](const OverlayFile& file, const index::Relation& r) {
                           if(!should_serve_overlay_file(file.path))
                               return true;
                           // to_uri canonicalizes clang's raw spelling (drive
                           // case) before emitting.
                           auto uri = feature::to_uri(file.path);
                           IndexedLineMap map(file.content, file.content_size, file.line_starts);
                           if(auto range = map.to_range(r.range.begin, r.range.end))
                               locations.push_back({uri, *range});
                           return true;
                       });
        return true;
    });

    // Preamble entries: the buffers' own preamble regions.
    visit_preambles([&](std::uint32_t id, const Session& session, const index::TUIndex& state) {
        auto uri = lsp::URI::from_file_path(std::string(workspace.path_pool.resolve(id)));
        if(!uri)
            return true;
        auto map = session.line_map();
        preamble_rows(state).lookup(hit.hash, kind, [&](const index::Relation& r) {
            if(auto range = map.to_range(r.range.begin, r.range.end))
                locations.push_back({uri->str(), *range});
            return true;
        });
        return true;
    });

    dedup_locations(locations);
    LOG_PERF("index_query",
             "kind=relations rel={} path={} results={} elapsed_ms={:.2f}",
             kota::meta::enum_name(static_cast<RelationKind::Kind>(kind), "Invalid"),
             path,
             locations.size(),
             timer.ms_f());
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
        session.file_rows().lookup(hash, RelationKind::Definition, [&](const index::Relation& r) {
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

    // PCH overlays outrank disk shards: they carry the definition as seen
    // under the live buffer's context, and exist even when no disk TU has
    // been indexed — the in-memory-file case behind empty go-to-definition.
    // First the buffers' own preamble regions, then the header entries.
    std::optional<protocol::Location> overlay_result;
    visit_preambles([&](std::uint32_t id, const Session& session, const index::TUIndex& state) {
        auto uri = lsp::URI::from_file_path(std::string(workspace.path_pool.resolve(id)));
        if(!uri)
            return true;
        auto map = session.line_map();
        preamble_rows(state).lookup(hash, RelationKind::Definition, [&](const index::Relation& r) {
            if(auto range = map.to_range(r.range.begin, r.range.end)) {
                overlay_result = protocol::Location{uri->str(), *range};
                return false;
            }
            return true;
        });
        return !overlay_result.has_value();
    });
    if(overlay_result)
        return overlay_result;

    visit_overlays([&](const index::TUIndex& state) {
        overlay_lookup(state,
                       hash,
                       RelationKind::Definition,
                       [&](const OverlayFile& file, const index::Relation& r) {
                           if(!should_serve_overlay_file(file.path))
                               return true;
                           auto uri = feature::to_uri(file.path);
                           IndexedLineMap map(file.content, file.content_size, file.line_starts);
                           if(auto range = map.to_range(r.range.begin, r.range.end)) {
                               overlay_result = protocol::Location{uri, *range};
                               return false;
                           }
                           return true;
                       });
        return !overlay_result.has_value();
    });
    if(overlay_result)
        return overlay_result;

    // Fall back to ProjectIndex reference files.
    auto sym_it = workspace.project_index.symbols.find(hash);
    if(sym_it == workspace.project_index.symbols.end())
        return std::nullopt;

    for(auto file_id: sym_it->second.reference_files) {
        if(skip_shard(file_id))
            continue;
        auto shard_it = workspace.shards.find(file_id);
        if(shard_it == workspace.shards.end())
            continue;
        auto uri = lsp::URI::from_file_path(workspace.path_pool.resolve(file_id));
        if(!uri)
            continue;
        auto& merged_index = shard_it->second;
        IndexedLineMap map(merged_index.content(),
                           merged_index.content_size(),
                           merged_index.line_starts());
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
            if(skip_shard(file_id))
                continue;
            auto shard_it = workspace.shards.find(file_id);
            if(shard_it == workspace.shards.end())
                continue;
            auto& merged_index = shard_it->second;
            IndexedLineMap map(merged_index.content(),
                               merged_index.content_size(),
                               merged_index.line_starts());
            merged_index.lookup(hash, kind, [&](const index::Relation& r) {
                if(auto range = map.to_range(r.range.begin, r.range.end))
                    target_ranges[r.target_symbol].push_back(*range);
                return true;
            });
        }
    }
    visit_sessions([&](std::uint32_t, const Session& session) -> bool {
        auto map = session.line_map();
        session.file_rows().lookup(hash, kind, [&](const index::Relation& r) {
            if(auto range = map.to_range(r.range.begin, r.range.end))
                target_ranges[r.target_symbol].push_back(*range);
            return true;
        });
        return true;
    });

    // PCH overlays: call/type relations inside headers under an open
    // buffer's context. The main-file entry cannot contribute — the
    // preamble region holds only preprocessor directives.
    visit_overlays([&](const index::TUIndex& state) {
        overlay_lookup(state, hash, kind, [&](const OverlayFile& file, const index::Relation& r) {
            if(!should_serve_overlay_file(file.path))
                return true;
            IndexedLineMap map(file.content, file.content_size, file.line_starts);
            if(auto range = map.to_range(r.range.begin, r.range.end))
                target_ranges[r.target_symbol].push_back(*range);
            return true;
        });
        return true;
    });

    // A row present in both a shard and an overlay lands twice; hierarchy
    // items must not repeat call sites.
    auto key = [](const protocol::Range& range) {
        return std::tie(range.start.line,
                        range.start.character,
                        range.end.line,
                        range.end.character);
    };
    for(auto& [target, ranges]: target_ranges) {
        std::ranges::sort(ranges,
                          [&](const auto& lhs, const auto& rhs) { return key(lhs) < key(rhs); });
        auto dup = std::ranges::unique(ranges, [&](const auto& lhs, const auto& rhs) {
            return key(lhs) == key(rhs);
        });
        ranges.erase(dup.begin(), dup.end());
    }
}

void IndexQuery::collect_unique_targets(index::SymbolHash hash,
                                        RelationKind kind,
                                        llvm::SmallVectorImpl<index::SymbolHash>& targets) {
    llvm::DenseSet<index::SymbolHash> seen;
    auto sym_it = workspace.project_index.symbols.find(hash);
    if(sym_it != workspace.project_index.symbols.end()) {
        for(auto file_id: sym_it->second.reference_files) {
            if(skip_shard(file_id))
                continue;
            auto shard_it = workspace.shards.find(file_id);
            if(shard_it == workspace.shards.end())
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
        session.file_rows().lookup(hash, kind, [&](const index::Relation& r) {
            if(seen.insert(r.target_symbol).second) {
                targets.push_back(r.target_symbol);
            }
            return true;
        });
        return true;
    });

    // PCH overlays follow the same file rules as every other consumer: an
    // open header's session is authoritative for its relations (an edited
    // `struct D : NewBase` must not resurface the disk snapshot's OldBase
    // through another file's overlay).
    visit_overlays([&](const index::TUIndex& state) {
        overlay_lookup(state, hash, kind, [&](const OverlayFile& file, const index::Relation& r) {
            if(!should_serve_overlay_file(file.path))
                return true;
            if(seen.insert(r.target_symbol).second) {
                targets.push_back(r.target_symbol);
            }
            return true;
        });
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

/// The stored text of an indexed file, for preview slicing. Non-ASCII
/// shards lend out the content they store; ASCII blobs do not store it:
/// re-read the disk into `storage` and serve it only while its hash
/// still matches what the rows were built from — a moved-on file
/// degrades to no preview rather than slicing mismatched text.
static std::optional<llvm::StringRef> indexed_text(llvm::StringRef path,
                                                   const index::Shard& shard,
                                                   std::unique_ptr<llvm::MemoryBuffer>& storage) {
    if(!shard.ascii()) {
        return shard.content();
    }
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return std::nullopt;
    }
    auto text = (*buffer)->getBuffer();
    if(llvm::xxh3_64bits(text) != shard.content_hash()) {
        return std::nullopt;
    }
    storage = std::move(*buffer);
    return text;
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
    auto sym_it = workspace.project_index.symbols.find(hash);
    if(sym_it == workspace.project_index.symbols.end())
        return std::nullopt;

    for(auto file_id: sym_it->second.reference_files) {
        if(skip_shard(file_id))
            continue;
        auto shard_it = workspace.shards.find(file_id);
        if(shard_it == workspace.shards.end())
            continue;
        auto& merged_index = shard_it->second;
        auto file_path = workspace.path_pool.resolve(file_id);
        std::unique_ptr<llvm::MemoryBuffer> storage;
        auto text = indexed_text(file_path, merged_index, storage);
        if(!text)
            continue;
        llvm::StringRef content = *text;
        lsp::LineMap map(content, merged_index.line_starts());

        std::optional<DefinitionText> result;
        merged_index.lookup(hash, RelationKind::Definition, [&](const index::Relation& r) {
            auto def_range = std::bit_cast<LocalSourceRange>(r.target_symbol);
            if(def_range.begin >= def_range.end || def_range.end > content.size())
                return true;
            auto range = map.to_range(def_range.begin, def_range.end);
            if(!range)
                return true;
            result = DefinitionText{
                .file = file_path.str(),
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
            if(skip_shard(file_id))
                continue;
            auto shard_it = workspace.shards.find(file_id);
            if(shard_it == workspace.shards.end())
                continue;
            auto& merged_index = shard_it->second;
            auto file_path = workspace.path_pool.resolve(file_id);
            // A moved-on ASCII file yields no text: positions still map
            // through the line table, only the context line degrades.
            std::unique_ptr<llvm::MemoryBuffer> storage;
            auto text = indexed_text(file_path, merged_index, storage);
            llvm::StringRef content = text.value_or(llvm::StringRef());
            IndexedLineMap map(content, merged_index.content_size(), merged_index.line_starts());

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
    ScopedTimer timer;
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
        session.index.iterate_symbols(
            [&](index::SymbolHash hash, const index::SymbolIdentity& symbol, llvm::StringRef) {
                if(results.size() >= max_results)
                    return false;
                if(seen.contains(hash))
                    return true;
                if(!is_indexable_kind(symbol.kind) || symbol.name.empty())
                    return true;
                if(!matches_query(symbol.name))
                    return true;
                auto def_loc = find_definition_location(hash);
                if(!def_loc)
                    return true;

                protocol::SymbolInformation info;
                info.name = std::string(symbol.name);
                info.kind = to_lsp_symbol_kind(symbol.kind);
                info.location = std::move(*def_loc);
                results.push_back(std::move(info));
                seen.insert(hash);
                return true;
            });
        return true;
    });
    // The query is arbitrary LSP input; its length is logged instead of its
    // text, which could contain newlines or `key=` fragments and corrupt
    // the key/value record.
    LOG_PERF("index_query",
             "kind=search query_len={} results={} elapsed_ms={:.2f}",
             query.size(),
             results.size(),
             timer.ms_f());
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

        if(!exact_matches.empty())
            return exact_matches;
        return candidates;
    }

    if(loc.path.has_value() && loc.line.has_value()) {
        auto path_str = *loc.path;
        auto target_line = static_cast<protocol::uinteger>(*loc.line - 1);

        auto path_id = workspace.path_pool.find(path_str);
        if(!path_id)
            return {};

        // The shard's line numbers describe stale text; resolving the
        // requested line against them would name the wrong symbol.
        if(skip_stale_contribution(*path_id))
            return {};

        auto shard_it = workspace.shards.find(*path_id);
        if(shard_it == workspace.shards.end())
            return {};

        auto& merged_index = shard_it->second;
        IndexedLineMap map(merged_index.content(),
                           merged_index.content_size(),
                           merged_index.line_starts());

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
