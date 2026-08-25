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
#include "llvm/ADT/STLExtras.h"
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
        if(session.index_current()) {
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

/// Whether a location names the very occurrence site the cursor stands on.
/// Occurrences and self-relations are written from the same record with
/// identical ranges — names spelled in macro arguments included, since
/// both anchor at the spelling — so an exact compare suffices.
static bool is_cursor_site(const protocol::Location& location,
                           llvm::StringRef uri,
                           const protocol::Range& range) {
    return location.uri == uri && location.range.start.line == range.start.line &&
           location.range.start.character == range.start.character &&
           location.range.end.line == range.end.line &&
           location.range.end.character == range.end.character;
}

/// Drop the cursor's own site from an answer set — standing on a
/// declaration or definition navigates to the other sites — unless it is
/// the only site the symbol has (an inline definition, nowhere else to go).
static void drop_cursor_site(std::vector<protocol::Location>& locations,
                             llvm::StringRef uri,
                             const protocol::Range& range) {
    if(locations.size() > 1) {
        std::erase_if(locations, [&](const protocol::Location& location) {
            return is_cursor_site(location, uri, range);
        });
    }
}

bool IndexQuery::skip_shard(std::uint32_t path_id) const {
    if(options.disk_only) {
        return skip_stale_contribution(path_id);
    }
    auto session = sessions.find(path_id);
    if(!session) {
        return skip_stale_contribution(path_id);
    }
    if(session->index_current()) {
        return true;
    }
    // Freshness contract, clause 4: an open document without a current
    // file index is served by its shard as if closed, while the buffer is
    // byte-identical to the content the rows were built from. The content
    // gate replaces the stale-contribution check — it compares against
    // the buffer being served, which is stricter than any disk baseline —
    // and a diverged buffer (edit in flight, restored unsaved text)
    // contributes nothing, exactly as before this clause.
    auto it = workspace.shards.find(path_id);
    return it == workspace.shards.end() || !it->second.matches_content(session->text);
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
    // Freshness contract, clause 1: an open document with a current file
    // index resolves against it — the compile the caller awaited settled
    // it together with the buffer.
    if(session && session->index_current()) {
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

    // Fallback to the disk shard. Freshness contract, clause 4: an open
    // document without a current file index resolves against its shard
    // while the buffer is byte-identical to the rows' content — the gate
    // that keeps a diverged buffer (edit in flight, restored unsaved
    // text) from the mixed-view lookup the contract exists to prevent.
    // Closed files keep the stale-contribution gate against their disk
    // baseline instead.
    auto path_id = workspace.path_pool.find(path);
    if(!path_id)
        return {};
    if(!session && skip_stale_contribution(*path_id))
        return {};
    auto shard_it = workspace.shards.find(*path_id);
    if(shard_it == workspace.shards.end())
        return {};

    auto& merged_index = shard_it->second;
    if(session && !merged_index.matches_content(session->text))
        return {};
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

std::vector<protocol::Location> IndexQuery::collect_relation_locations(index::SymbolHash hash,
                                                                       RelationKind kind) {
    std::vector<protocol::Location> locations;

    auto sym_it = workspace.project_index.symbols.find(hash);
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
            merged_index.lookup(hash, kind, [&](const index::Relation& r) {
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
        session.file_rows().lookup(hash, kind, [&](const index::Relation& r) {
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
        overlay_lookup(state, hash, kind, [&](const OverlayFile& file, const index::Relation& r) {
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
        preamble_rows(state).lookup(hash, kind, [&](const index::Relation& r) {
            if(auto range = map.to_range(r.range.begin, r.range.end))
                locations.push_back({uri->str(), *range});
            return true;
        });
        return true;
    });

    // Same-kind rows can share one anchor: a macro body using an
    // argument twice spells both references at the one written token.
    dedup_locations(locations);
    return locations;
}

std::vector<protocol::Location> IndexQuery::query_relations(llvm::StringRef path,
                                                            const protocol::Position& position,
                                                            RelationKind kind,
                                                            Session* session) {
    ScopedTimer timer;
    auto hit = resolve_cursor(path, position, session);
    std::vector<protocol::Location> locations;
    if(hit.hash != 0) {
        locations = collect_relation_locations(hit.hash, kind);
    }
    // Misses (whitespace, comments, unindexed positions) are normal
    // inputs; their latency belongs in the series like any hit's.
    LOG_PERF("index_query",
             "kind=relations rel={} path={} results={} elapsed_ms={:.2f}",
             kota::meta::enum_name(static_cast<RelationKind::Kind>(kind), "Invalid"),
             path,
             locations.size(),
             timer.ms_f());
    return locations;
}

std::vector<protocol::Location> IndexQuery::query_references(llvm::StringRef path,
                                                             const protocol::Position& position,
                                                             bool include_declaration,
                                                             Session* session) {
    ScopedTimer timer;
    auto hit = resolve_cursor(path, position, session);
    std::vector<protocol::Location> locations;
    if(hit.hash != 0) {
        locations = collect_relation_locations(hit.hash, RelationKind::Reference);
        if(include_declaration) {
            for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
                auto extra = collect_relation_locations(hit.hash, kind);
                locations.insert(locations.end(),
                                 std::make_move_iterator(extra.begin()),
                                 std::make_move_iterator(extra.end()));
            }
        }
        dedup_locations(locations);
    }
    LOG_PERF("index_query",
             "kind=references path={} results={} elapsed_ms={:.2f}",
             path,
             locations.size(),
             timer.ms_f());
    return locations;
}

std::string IndexQuery::self_uri(llvm::StringRef path) {
    // Collected locations spell URIs from the pool's canonical form; the
    // transport's raw path can differ (drive case on Windows), which would
    // defeat cursor-site detection.
    auto path_id = workspace.path_pool.find(path);
    if(!path_id)
        return {};
    auto uri = lsp::URI::from_file_path(workspace.path_pool.resolve(*path_id));
    return uri ? uri->str() : std::string{};
}

std::vector<protocol::Location> IndexQuery::query_definition(llvm::StringRef path,
                                                             const protocol::Position& position,
                                                             Session* session) {
    ScopedTimer timer;
    auto hit = resolve_cursor(path, position, session);
    std::vector<protocol::Location> locations;
    if(hit.hash != 0) {
        auto self = self_uri(path);
        locations = collect_relation_locations(hit.hash, RelationKind::Definition);
        if(locations.empty() || std::ranges::any_of(locations, [&](const protocol::Location& l) {
               return is_cursor_site(l, self, hit.range);
           })) {
            auto decls = collect_relation_locations(hit.hash, RelationKind::Declaration);
            locations.insert(locations.end(),
                             std::make_move_iterator(decls.begin()),
                             std::make_move_iterator(decls.end()));
            dedup_locations(locations);
            drop_cursor_site(locations, self, hit.range);
        }
    }
    LOG_PERF("index_query",
             "kind=definition path={} results={} elapsed_ms={:.2f}",
             path,
             locations.size(),
             timer.ms_f());
    return locations;
}

std::vector<protocol::Location> IndexQuery::query_declaration(llvm::StringRef path,
                                                              const protocol::Position& position,
                                                              Session* session) {
    ScopedTimer timer;
    auto hit = resolve_cursor(path, position, session);
    std::vector<protocol::Location> locations;
    if(hit.hash != 0) {
        locations = collect_relation_locations(hit.hash, RelationKind::Declaration);
        auto defs = collect_relation_locations(hit.hash, RelationKind::Definition);
        locations.insert(locations.end(),
                         std::make_move_iterator(defs.begin()),
                         std::make_move_iterator(defs.end()));
        dedup_locations(locations);
        drop_cursor_site(locations, self_uri(path), hit.range);
    }
    LOG_PERF("index_query",
             "kind=declaration path={} results={} elapsed_ms={:.2f}",
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

    return resolve_target_locations(hit.hash, kind);
}

std::vector<protocol::Location> IndexQuery::query_implementation(llvm::StringRef path,
                                                                 const protocol::Position& position,
                                                                 Session* session) {
    auto hit = resolve_cursor(path, position, session);
    if(hit.hash == 0)
        return {};

    std::string name;
    SymbolKind kind;
    if(!find_symbol_info(hit.hash, name, kind))
        return {};

    bool type_like =
        kind == SymbolKind::Class || kind == SymbolKind::Struct || kind == SymbolKind::Union;
    return resolve_target_locations(hit.hash,
                                    type_like ? RelationKind::Derived
                                              : RelationKind::Implementation);
}

std::vector<protocol::Location> IndexQuery::resolve_target_locations(index::SymbolHash hash,
                                                                     RelationKind kind) {
    llvm::SmallVector<index::SymbolHash> targets;
    collect_unique_targets(hash, kind, targets);

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

std::optional<protocol::Location> IndexQuery::find_relation_location(index::SymbolHash hash,
                                                                     RelationKind kind) {
    std::optional<protocol::Location> session_result;
    visit_sessions([&](std::uint32_t id, const Session& session) -> bool {
        auto uri = lsp::URI::from_file_path(std::string(workspace.path_pool.resolve(id)));
        if(!uri)
            return true;
        auto map = session.line_map();
        session.file_rows().lookup(hash, kind, [&](const index::Relation& r) {
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
        preamble_rows(state).lookup(hash, kind, [&](const index::Relation& r) {
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
        overlay_lookup(state, hash, kind, [&](const OverlayFile& file, const index::Relation& r) {
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
        merged_index.lookup(hash, kind, [&](const index::Relation& r) {
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

std::optional<protocol::Location> IndexQuery::find_definition_location(index::SymbolHash hash) {
    return find_relation_location(hash, RelationKind::Definition);
}

std::optional<protocol::Location> IndexQuery::find_symbol_location(index::SymbolHash hash) {
    if(auto location = find_relation_location(hash, RelationKind::Definition))
        return location;
    return find_relation_location(hash, RelationKind::Declaration);
}

std::optional<SymbolInfo>
    IndexQuery::resolve_hierarchy_item(const std::string& uri,
                                       llvm::StringRef path,
                                       const protocol::Range& range,
                                       const std::optional<protocol::LSPAny>& data,
                                       Session* session) {
    if(data) {
        if(auto* str = std::get_if<std::string>(&*data)) {
            index::SymbolHash hash = 0;
            if(!llvm::StringRef(*str).getAsInteger(10, hash)) {
                std::string name;
                SymbolKind kind;
                if(find_symbol_info(hash, name, kind)) {
                    return SymbolInfo{hash, std::move(name), kind, uri, range};
                }
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
    auto location = find_symbol_location(hash);
    if(!location)
        return std::nullopt;
    return SymbolInfo{hash, std::move(name), kind, location->uri, location->range};
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

/// A definition's extent within one shard's stored content, or nullopt
/// when the shard has no in-bounds Definition payload for the symbol.
static std::optional<LocalSourceRange> definition_extent(const index::Shard& shard,
                                                         index::SymbolHash hash,
                                                         llvm::StringRef content) {
    std::optional<LocalSourceRange> extent;
    shard.lookup(hash, RelationKind::Definition, [&](const index::Relation& r) {
        auto def_range = std::bit_cast<LocalSourceRange>(r.target_symbol);
        if(def_range.begin >= def_range.end || def_range.end > content.size())
            return true;
        extent = def_range;
        return false;
    });
    return extent;
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
        auto extent = definition_extent(merged_index, hash, content);
        if(!extent)
            continue;

        lsp::LineMap map(content, merged_index.line_starts());
        auto range = map.to_range(extent->begin, extent->end);
        if(!range)
            continue;
        return DefinitionText{
            .file = file_path.str(),
            .start_line = static_cast<int>(range->start.line) + 1,
            .end_line = static_cast<int>(range->end.line) + 1,
            .text = std::string(content.substr(extent->begin, extent->length())),
        };
    }

    return std::nullopt;
}

const index::Shard* IndexQuery::open_session_shard(const Session& session) const {
    if(session.index_current()) {
        return nullptr;
    }
    auto it = workspace.shards.find(session.path_id);
    if(it == workspace.shards.end() || !it->second.matches_content(session.text)) {
        return nullptr;
    }
    return &it->second;
}

std::vector<feature::IndexIncludeEdge> IndexQuery::include_edges(const Session& session) const {
    auto& project = workspace.project_index;

    // Every consumer projects the edges onto content the serving shard
    // matches, so a manifest contributes only where the version it entered
    // for this document carries that same content generation — a TU that
    // indexed an older revision would place its lines in text that moved.
    auto shard_it = workspace.shards.find(session.path_id);
    if(shard_it == workspace.shards.end()) {
        return {};
    }
    auto generation = shard_it->second.content_hash();

    auto version_of = [&](std::uint32_t fv) -> const index::FileVersionRecord* {
        auto it = project.file_versions.find(fv);
        return it == project.file_versions.end() ? nullptr : &it->second;
    };
    auto is_document = [&](std::uint32_t fv) {
        const auto* version = version_of(fv);
        return version && version->path_id == session.path_id &&
               version->content_hash == generation;
    };

    // A directive line of the document is a node whose parent node entered
    // this file: the TU root (parent == ~0u) when the document is the TU
    // itself, or any node of the document's own file version otherwise
    // (directives inside included headers hang off the node of the file
    // that contains them).
    std::vector<feature::IndexIncludeEdge> edges;
    auto append = [&](const index::TUManifest& manifest) {
        bool root_is_document = is_document(manifest.tu_fv);
        llvm::SmallVector<bool> document_nodes(manifest.nodes.size());
        for(auto [i, node]: llvm::enumerate(manifest.nodes)) {
            document_nodes[i] = is_document(node.fv);
        }
        for(const auto& node: manifest.nodes) {
            if(node.parent == ~0u ? !root_is_document : !document_nodes[node.parent]) {
                continue;
            }
            const auto* target = version_of(node.fv);
            if(!target) {
                continue;
            }
            edges.push_back({
                .line = node.line,
                .target = std::string(workspace.path_pool.resolve(target->path_id)),
            });
        }
    };

    // The document's own manifest is its own context and answers alone; a
    // header reached only through source TUs has none, and its directives
    // live in the contributing TUs' manifests instead. The generation gate
    // above dedups divergent revisions; agreeing TUs collapse in the
    // projection's dedup.
    if(auto manifest_it = project.manifests.find(session.path_id);
       manifest_it != project.manifests.end()) {
        append(manifest_it->second);
        return edges;
    }
    if(auto contribution_it = project.contributions.find(session.path_id);
       contribution_it != project.contributions.end()) {
        for(auto tu: llvm::make_first_range(contribution_it->second)) {
            if(auto manifest_it = project.manifests.find(tu);
               manifest_it != project.manifests.end()) {
                append(manifest_it->second);
            }
        }
    }
    return edges;
}

std::optional<feature::HoverInfo> IndexQuery::hover_card(llvm::StringRef path,
                                                         const protocol::Position& position,
                                                         Session* session) {
    auto hit = resolve_cursor(path, position, session);
    if(hit.hash == 0) {
        return std::nullopt;
    }

    feature::IndexSymbolInfo info;
    if(!find_symbol_info(hit.hash, info.name, info.kind)) {
        return std::nullopt;
    }

    // Definition text and its comment block, preferring the document's own
    // serving source (buffer-true content); external symbols defined
    // elsewhere fall back to the defining file's stored content.
    std::string definition;
    std::string comment;
    auto slice_from = [&](const index::Shard& shard, llvm::StringRef content) {
        auto extent = definition_extent(shard, hit.hash, content);
        if(!extent) {
            return false;
        }
        definition = std::string(content.substr(extent->begin, extent->length()));
        comment = feature::preceding_comment(content, extent->begin);
        return true;
    };

    bool sliced = false;
    if(session) {
        if(session->index_current()) {
            sliced = slice_from(session->file_rows(), session->text);
        } else if(auto* shard = open_session_shard(*session)) {
            sliced = slice_from(*shard, session->text);
        }
    }
    if(!sliced) {
        if(auto sym_it = workspace.project_index.symbols.find(hit.hash);
           sym_it != workspace.project_index.symbols.end()) {
            for(auto file_id: sym_it->second.reference_files) {
                // A defining file that is itself open serves through its
                // session sources — the very reason skip_shard rejects
                // its disk shard — so slice from those buffer-true rows
                // instead of losing the definition text.
                if(!options.disk_only) {
                    if(auto other = sessions.find(file_id)) {
                        if(other->index_current() && slice_from(other->file_rows(), other->text))
                            break;
                        if(auto* shard = open_session_shard(*other);
                           shard && slice_from(*shard, other->text))
                            break;
                    }
                }
                if(skip_shard(file_id))
                    continue;
                auto shard_it = workspace.shards.find(file_id);
                if(shard_it == workspace.shards.end())
                    continue;
                std::unique_ptr<llvm::MemoryBuffer> storage;
                auto text =
                    indexed_text(workspace.path_pool.resolve(file_id), shard_it->second, storage);
                if(text && slice_from(shard_it->second, *text))
                    break;
            }
        }
    }

    auto hover = feature::index_hover(info, definition, comment);
    if(session) {
        auto map = session->line_map();
        auto begin = map.to_offset(hit.range.start);
        auto end = map.to_offset(hit.range.end);
        if(begin && end) {
            hover.symbol_range = LocalSourceRange{*begin, *end};
        }
    }
    return hover;
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

/// The symbol handle travels as a decimal string: a 64-bit integer would
/// be parsed into a double by a JavaScript client and come back rounded.
protocol::CallHierarchyItem IndexQuery::build_call_hierarchy_item(const SymbolInfo& info) {
    protocol::CallHierarchyItem item;
    item.name = info.name;
    item.kind = to_lsp_symbol_kind(info.kind);
    item.uri = info.uri;
    item.range = info.range;
    item.selection_range = info.range;
    item.data = protocol::LSPAny(std::format("{}", info.hash));
    return item;
}

protocol::TypeHierarchyItem IndexQuery::build_type_hierarchy_item(const SymbolInfo& info) {
    protocol::TypeHierarchyItem item;
    item.name = info.name;
    item.kind = to_lsp_symbol_kind(info.kind);
    item.uri = info.uri;
    item.range = info.range;
    item.selection_range = info.range;
    item.data = protocol::LSPAny(std::format("{}", info.hash));
    return item;
}

}  // namespace clice
