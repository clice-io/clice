#include "server/service/query.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <string>
#include <tuple>
#include <vector>

#include "index/tu_index.h"
#include "sched/index/pump.h"
#include "server/state/ast_projection.h"
#include "server/state/session.h"
#include "server/state/session_store.h"
#include "support/logging.h"
#include "support/timer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice {

/// One readable row set with its own coordinate system, as the federation
/// hands it to a visitor.
struct RowSource {
    enum class Kind : std::uint8_t { Shard, SessionRows, PreambleRows, Overlay };

    Kind kind;
    Fid file;
    llvm::StringRef path;
    const index::Shard* rows;
    Coordinates coords;

    Site site(LocalSourceRange range) const {
        assert(range.begin <= range.end && range.end <= coords.size() &&
               "served rows lie within their source's text");
        return {.file = file, .path = path, .range = range, .coords = coords};
    }
};

namespace {

Coordinates shard_coordinates(const index::Shard& shard) {
    return {shard.content(), shard.content_size(), shard.line_starts()};
}

Coordinates buffer_coordinates(const Session& session) {
    return {session.text, static_cast<std::uint32_t>(session.text.size()), session.line_starts};
}

/// The source file's preamble-region rows of an overlay envelope (buffer
/// offsets below the preamble bound).
const index::Shard& preamble_rows(const index::TUIndex& state) {
    return state.shard_of(state.path_count() - 1);
}

LocalSourceRange to_local(const index::Occurrence& occurrence) {
    return {occurrence.range.begin, occurrence.range.end};
}

std::string extract_line(llvm::StringRef content, std::uint32_t offset) {
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

/// The disk bytes of a pure-ASCII blob's file, served only while their
/// hash still matches what the rows were built from — a moved-on file
/// degrades to no text rather than slicing mismatched text.
std::optional<llvm::StringRef> disk_text(llvm::StringRef path,
                                         const index::Shard& shard,
                                         std::unique_ptr<llvm::MemoryBuffer>& storage) {
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

auto site_key(const Site& site) {
    return std::tie(site.path, site.range.begin, site.range.end);
}

bool same_site(const Site& lhs, const Site& rhs) {
    return lhs.path == rhs.path && lhs.range == rhs.range;
}

/// Cross-source dedup: a row present in both a disk shard and a PCH
/// overlay (or in two overlays sharing a preamble) comes out identical.
void dedup_sites(std::vector<Site>& sites) {
    std::ranges::sort(sites, [](const Site& lhs, const Site& rhs) {
        return site_key(lhs) < site_key(rhs);
    });
    auto dup = std::ranges::unique(sites, [](const Site& lhs, const Site& rhs) {
        return site_key(lhs) == site_key(rhs);
    });
    sites.erase(dup.begin(), dup.end());
}

/// Drop the cursor's own site from an answer set — standing on a
/// declaration or definition navigates to the other sites — unless it is
/// the only site the symbol has (an inline definition, nowhere else to go).
/// Occurrences and self-relations are written from the same record with
/// identical ranges, so an exact compare suffices.
void drop_cursor_site(std::vector<Site>& sites, const Site& cursor) {
    if(sites.size() > 1) {
        std::erase_if(sites, [&](const Site& site) { return same_site(site, cursor); });
    }
}

SymbolRef to_ref(index::SymbolHash hash, const index::Symbol& symbol) {
    return {
        .hash = hash,
        .name = symbol.name,
        .args = symbol.args,
        .parent = symbol.parent,
        .kind = symbol.kind,
        .flags = symbol.flags,
    };
}

/// Name matching for searches: a symbol is looked up by its displayed
/// name, so a specialization answers to `Box<int>` as well as `Box`.
/// Most symbols carry no arguments and match on the stored name alone.
bool name_contains(const index::Symbol& symbol, llvm::StringRef query_lower) {
    if(symbol.args.empty()) {
        return llvm::StringRef(symbol.name).lower().find(query_lower) != std::string::npos;
    }
    return llvm::StringRef(symbol.name + symbol.args).lower().find(query_lower) !=
           std::string::npos;
}

bool is_indexable_kind(SymbolKind kind) {
    return kind == SymbolKind::Namespace || kind == SymbolKind::Class ||
           kind == SymbolKind::Struct || kind == SymbolKind::Union || kind == SymbolKind::Enum ||
           kind == SymbolKind::Type || kind == SymbolKind::Field ||
           kind == SymbolKind::EnumMember || kind == SymbolKind::Function ||
           kind == SymbolKind::Method || kind == SymbolKind::Variable ||
           kind == SymbolKind::Parameter || kind == SymbolKind::Macro ||
           kind == SymbolKind::Concept || kind == SymbolKind::Module ||
           kind == SymbolKind::Operator || kind == SymbolKind::MacroParameter ||
           kind == SymbolKind::Label || kind == SymbolKind::Attribute;
}

}  // namespace

IndexQuery::IndexQuery(Workspace& workspace, QuerySources sources) :
    workspace(workspace), sources(sources) {
    assert((!sources.sessions || sources.projections) &&
           "buffer rows need the projections that tell which are current");
}

bool IndexQuery::is_open(Fid file) const {
    return sources.sessions && sources.sessions->find(file) != nullptr;
}

bool IndexQuery::skip_stale_contribution(Fid file) const {
    // With background indexing disabled nothing ever catches up: serving
    // the last-known rows beats a permanent hole.
    if(!workspace.config.project.enable_indexing.value) {
        return false;
    }
    return sources.pump && sources.pump->pending_reason(file) == ReindexReason::ContentChanged;
}

ServingSource IndexQuery::serving_source(Fid file) const {
    auto session = sources.sessions ? sources.sessions->find(file) : nullptr;
    if(session) {
        if(sources.projections->index_current(file)) {
            auto projection = sources.projections->projection(file);
            return {.by = ServingSource::By::SessionRows,
                    .rows = &projection->file_rows(),
                    .coords = buffer_coordinates(*session)};
        }
        auto* shard = matching_shard(*session);
        if(!shard) {
            return {};
        }
        return {.by = ServingSource::By::ShardAsClosed,
                .rows = shard,
                .coords = buffer_coordinates(*session)};
    }
    if(skip_stale_contribution(file)) {
        return {};
    }
    auto it = workspace.shards.find(file);
    if(it == workspace.shards.end()) {
        return {};
    }
    return {.by = ServingSource::By::ShardAsClosed,
            .rows = &it->second,
            .coords = shard_coordinates(it->second)};
}

const index::Shard* IndexQuery::matching_shard(const Session& session) const {
    auto it = workspace.shards.find(session.path_id);
    if(it == workspace.shards.end() || !it->second.matches_content(session.text)) {
        return nullptr;
    }
    return &it->second;
}

std::shared_ptr<index::TUIndex> IndexQuery::overlay_of(const Session& session) const {
    auto projection = sources.projections->projection(session.path_id);
    if(!projection || !projection->pch_key) {
        return nullptr;
    }
    // Returned by value: a reference into the map value would not survive
    // a rehash.
    return workspace.preamble_state(*projection->pch_key);
}

std::shared_ptr<index::TUIndex> IndexQuery::preamble_blob(const Session& session) const {
    if(!sources.projections) {
        return nullptr;
    }
    auto state = overlay_of(session);
    if(!state || !state->matches_prefix(session.text)) {
        return nullptr;
    }
    return state;
}

void IndexQuery::visit_sessions(llvm::function_ref<bool(Fid, const Session&)> visitor) const {
    if(!sources.sessions) {
        return;
    }
    sources.sessions->for_each([&](Fid path_id, const Session& session) -> bool {
        if(sources.projections->index_current(path_id)) {
            return visitor(path_id, session);
        }
        return true;
    });
}

void IndexQuery::visit_overlays(llvm::function_ref<bool(const index::TUIndex&)> visitor) const {
    if(!sources.sessions) {
        return;
    }
    // Sessions with identical preambles share one blob; visit it once.
    llvm::StringSet<> seen;
    sources.sessions->for_each([&](Fid path_id, const Session& session) -> bool {
        auto projection = sources.projections->projection(path_id);
        if(!projection || !projection->pch_key || !seen.insert(*projection->pch_key).second) {
            return true;
        }
        auto state = overlay_of(session);
        return state ? visitor(*state) : true;
    });
}

void IndexQuery::visit_preambles(
    llvm::function_ref<bool(Fid, const Session&, const index::TUIndex&)> visitor) const {
    if(!sources.sessions) {
        return;
    }
    sources.sessions->for_each([&](Fid path_id, const Session& session) -> bool {
        auto state = overlay_of(session);
        if(!state) {
            return true;
        }
        // The preamble entry's rows are buffer offsets of the file that
        // built the blob: serve them only for that very file and only while
        // the buffer still starts with the exact preamble text the blob was
        // built from. The prefix comparison validates the described region
        // directly — body edits never move preamble rows — so no dirty-flag
        // gating is needed on top. The blob stores clang's native path
        // (backslashes on Windows) while the table normalizes separators,
        // so compare through the table's lookup, not raw strings.
        if(workspace.file_table.find(state->path(state->path_count() - 1)) != path_id ||
           !state->matches_prefix(session.text)) {
            return true;
        }
        return visitor(path_id, session, *state);
    });
}

void IndexQuery::visit_overlay_files(const index::TUIndex& state,
                                     llvm::function_ref<bool(const RowSource&)> visitor) const {
    auto main_id = state.path_count() - 1;
    for(std::uint32_t i = 0; i < state.section_count(); i += 1) {
        auto local_id = state.section_path(i);
        if(local_id == main_id) {
            continue;
        }
        auto path = state.path(local_id);
        Fid file;
        if(auto known = workspace.file_table.find(path)) {
            if(is_open(*known) || skip_stale_contribution(*known)) {
                continue;
            }
            file = *known;
            path = workspace.file_table.resolve(file);
        }
        if(workspace.is_synthesized_artifact(path)) {
            continue;
        }
        auto& shard = state.shard_of(local_id);
        RowSource source{.kind = RowSource::Kind::Overlay,
                         .file = file,
                         .path = path,
                         .rows = &shard,
                         .coords = shard_coordinates(shard)};
        if(!visitor(source)) {
            return;
        }
    }
}

RowSource IndexQuery::session_source(Fid path_id, const Session& session) const {
    return {.kind = RowSource::Kind::SessionRows,
            .file = path_id,
            .path = workspace.file_table.resolve(path_id),
            .rows = &sources.projections->projection(path_id)->file_rows(),
            .coords = buffer_coordinates(session)};
}

RowSource IndexQuery::preamble_source(Fid path_id,
                                      const Session& session,
                                      const index::TUIndex& state) const {
    return {.kind = RowSource::Kind::PreambleRows,
            .file = path_id,
            .path = workspace.file_table.resolve(path_id),
            .rows = &preamble_rows(state),
            .coords = buffer_coordinates(session)};
}

void IndexQuery::for_each_relation(index::SymbolHash hash,
                                   RelationKind kind,
                                   Order order,
                                   SourceMask mask,
                                   RelationVisitor visitor) const {
    bool stopped = false;
    auto emit = [&](const RowSource& source) {
        source.rows->lookup(hash, kind, [&](const index::Relation& relation) {
            if(!visitor(source, relation)) {
                stopped = true;
                return false;
            }
            return true;
        });
        return !stopped;
    };

    auto disk = [&] {
        if(!mask.shard) {
            return true;
        }
        auto it = workspace.project_index.symbols.find(hash);
        if(it == workspace.project_index.symbols.end()) {
            return true;
        }
        for(auto file_id: it->second.reference_files) {
            Fid file{file_id};
            auto serving = serving_source(file);
            if(serving.by != ServingSource::By::ShardAsClosed) {
                continue;
            }
            RowSource source{.kind = RowSource::Kind::Shard,
                             .file = file,
                             .path = workspace.file_table.resolve(file),
                             .rows = serving.rows,
                             .coords = serving.coords};
            if(!emit(source)) {
                return false;
            }
        }
        return true;
    };
    auto sessions = [&] {
        if(!mask.session) {
            return true;
        }
        visit_sessions([&](Fid path_id, const Session& session) -> bool {
            return emit(session_source(path_id, session));
        });
        return !stopped;
    };
    auto preambles = [&] {
        if(!mask.preamble) {
            return true;
        }
        visit_preambles([&](Fid path_id, const Session& session, const index::TUIndex& state) {
            return emit(preamble_source(path_id, session, state));
        });
        return !stopped;
    };
    auto overlays = [&] {
        if(!mask.overlay) {
            return true;
        }
        visit_overlays([&](const index::TUIndex& state) {
            visit_overlay_files(state, emit);
            return !stopped;
        });
        return !stopped;
    };

    if(order == Order::LiveFirst) {
        // Live sources outrank disk shards: they carry the rows as seen
        // under the buffer's context, and exist even when no disk TU has
        // been indexed — the in-memory-file case behind empty
        // go-to-definition. First the buffers' own rows, then their
        // preamble regions, then the header entries.
        [[maybe_unused]] bool completed = sessions() && preambles() && overlays() && disk();
    } else {
        [[maybe_unused]] bool completed = disk() && sessions() && overlays() && preambles();
    }
}

std::optional<IndexQuery::Cursor> IndexQuery::symbol_at(Fid file, std::uint32_t offset) const {
    auto serving = serving_source(file);
    if(!serving) {
        return std::nullopt;
    }
    auto path = workspace.file_table.resolve(file);
    std::optional<Cursor> cursor;
    auto hit = [&](const index::Shard& rows) {
        // Several symbols can share the name span: a module imported
        // through a macro sits under the macro's own occurrence. The
        // name spells what the macro expanded to; the macro itself is
        // reached at its definition.
        llvm::SmallVector<index::Occurrence, 2> candidates;
        rows.lookup(offset, [&](const index::Occurrence& occurrence) {
            if(!candidates.empty() && !(candidates.front().range == occurrence.range)) {
                return false;
            }
            candidates.push_back(occurrence);
            return true;
        });
        if(candidates.empty()) {
            return false;
        }
        auto chosen = candidates.front();
        if(candidates.size() > 1) {
            for(auto& candidate: candidates) {
                auto info = symbol_info(candidate.target);
                if(info && info->kind != SymbolKind::Macro) {
                    chosen = candidate;
                    break;
                }
            }
        }
        cursor = Cursor{
            .symbol = chosen.target,
            .site = {.file = file,
                     .path = path,
                     .range = to_local(chosen),
                     .coords = serving.coords},
        };
        return true;
    };
    if(hit(*serving.rows)) {
        return cursor;
    }
    if(serving.by != ServingSource::By::SessionRows) {
        return std::nullopt;
    }
    // The preamble region is compiled into the PCH and invisible to the
    // per-edit index; its occurrences (macro definitions and references
    // before the bound) live in the PCH's overlay, in the same buffer
    // coordinates — served only under the main-entry gate (preamble
    // drift, shared-PCH identity).
    visit_preambles([&](Fid path_id, const Session&, const index::TUIndex& state) {
        if(path_id != file) {
            return true;
        }
        hit(preamble_rows(state));
        return false;
    });
    return cursor;
}

std::optional<SymbolRef> IndexQuery::symbol_info(index::SymbolHash hash) const {
    std::optional<SymbolRef> found;
    auto adopt = [&](const index::SymbolIdentity& identity) {
        found = SymbolRef{
            .hash = hash,
            .name = std::string(identity.name),
            .args = std::string(identity.args),
            .parent = identity.parent,
            .kind = identity.kind,
            .flags = identity.flags,
        };
    };

    // Open sessions first: they hold every symbol of their unsaved buffers.
    visit_sessions([&](Fid path_id, const Session&) -> bool {
        if(auto identity = sources.projections->projection(path_id)->index->find_symbol(hash)) {
            adopt(*identity);
            return false;
        }
        return true;
    });
    if(found) {
        return found;
    }

    auto it = workspace.project_index.symbols.find(hash);
    if(it != workspace.project_index.symbols.end()) {
        return to_ref(hash, it->second);
    }

    // A symbol that exists only under an open buffer's context (or in
    // headers no disk TU has been indexed with) is in no disk table.
    visit_overlays([&](const index::TUIndex& state) {
        if(auto identity = state.find_symbol(hash)) {
            adopt(*identity);
        }
        return !found;
    });
    if(found) {
        return found;
    }

    // Each shard stores exactly the local symbols its occurrences
    // reference, so a TU-local name is in the shard that produced it.
    for(auto& [path_id, shard]: workspace.shards) {
        if(auto identity = shard.find_symbol(hash)) {
            adopt(*identity);
            return found;
        }
    }
    return std::nullopt;
}

std::string IndexQuery::container_name(index::SymbolHash hash) const {
    auto symbol = symbol_info(hash);
    if(!symbol) {
        return {};
    }
    llvm::SmallVector<std::string, 4> scopes;
    // A parent chain follows declaration contexts, so it is acyclic as
    // built; the guard keeps a corrupted parent column from spinning.
    llvm::DenseSet<index::SymbolHash> visited{hash};
    for(auto parent = symbol->parent; parent != 0 && visited.insert(parent).second;) {
        auto scope = symbol_info(parent);
        if(!scope) {
            break;
        }
        if(!index::has_flag(scope->flags, index::SymbolFlags::InlineNamespace)) {
            scopes.push_back(scope->display_name());
        }
        parent = scope->parent;
    }
    std::string result;
    for(auto& scope: llvm::reverse(scopes)) {
        if(!result.empty()) {
            result += "::";
        }
        result += scope;
    }
    return result;
}

std::string IndexQuery::qualified_name(index::SymbolHash hash) const {
    auto symbol = symbol_info(hash);
    if(!symbol) {
        return {};
    }
    auto container = container_name(hash);
    if(container.empty()) {
        return symbol->display_name();
    }
    return container + "::" + symbol->display_name();
}

std::vector<Site> IndexQuery::sites(index::SymbolHash hash, RelationKind kind) const {
    std::vector<Site> result;
    for_each_relation(hash,
                      kind,
                      Order::DiskFirst,
                      {},
                      [&](const RowSource& source, const index::Relation& relation) {
                          result.push_back(source.site(relation.range));
                          return true;
                      });
    // Same-kind rows can share one anchor: a macro body using an argument
    // twice spells both references at the one written token.
    dedup_sites(result);
    return result;
}

std::optional<Site> IndexQuery::first_site(index::SymbolHash hash, RelationKind kind) const {
    std::optional<Site> result;
    for_each_relation(hash,
                      kind,
                      Order::LiveFirst,
                      {},
                      [&](const RowSource& source, const index::Relation& relation) {
                          result = source.site(relation.range);
                          return false;
                      });
    return result;
}

std::optional<Site> IndexQuery::canonical_site(index::SymbolHash hash) const {
    if(auto site = first_site(hash, RelationKind::Definition)) {
        return site;
    }
    return first_site(hash, RelationKind::Declaration);
}

std::vector<IndexQuery::Group> IndexQuery::grouped(index::SymbolHash hash,
                                                   RelationKind kind) const {
    // The main-file preamble entry cannot contribute: the preamble region
    // holds only preprocessor directives, never call or type relations.
    llvm::DenseMap<index::SymbolHash, std::vector<Site>> by_target;
    for_each_relation(hash,
                      kind,
                      Order::DiskFirst,
                      {.preamble = false},
                      [&](const RowSource& source, const index::Relation& relation) {
                          by_target[relation.target_symbol].push_back(source.site(relation.range));
                          return true;
                      });
    std::vector<Group> groups;
    groups.reserve(by_target.size());
    for(auto& [target, sites]: by_target) {
        // A row present in both a shard and an overlay lands twice;
        // hierarchy items must not repeat call sites.
        dedup_sites(sites);
        groups.push_back({.symbol = target, .sites = std::move(sites)});
    }
    return groups;
}

llvm::SmallVector<index::SymbolHash> IndexQuery::targets(index::SymbolHash hash,
                                                         RelationKind kind) const {
    llvm::SmallVector<index::SymbolHash> result;
    llvm::DenseSet<index::SymbolHash> seen;
    for_each_relation(hash,
                      kind,
                      Order::DiskFirst,
                      {.preamble = false},
                      [&](const RowSource&, const index::Relation& relation) {
                          if(seen.insert(relation.target_symbol).second) {
                              result.push_back(relation.target_symbol);
                          }
                          return true;
                      });
    return result;
}

std::vector<Site> IndexQuery::definition(const Cursor& cursor) const {
    ScopedTimer timer;
    auto result = sites(cursor.symbol, RelationKind::Definition);
    if(result.empty() || std::ranges::any_of(result, [&](const Site& site) {
           return same_site(site, cursor.site);
       })) {
        auto decls = sites(cursor.symbol, RelationKind::Declaration);
        result.insert(result.end(),
                      std::make_move_iterator(decls.begin()),
                      std::make_move_iterator(decls.end()));
        dedup_sites(result);
        drop_cursor_site(result, cursor.site);
    }
    LOG_PERF("index_query",
             "kind=definition path={} results={} elapsed_ms={:.2f}",
             cursor.site.path,
             result.size(),
             timer.ms_f());
    return result;
}

std::vector<Site> IndexQuery::declaration(const Cursor& cursor) const {
    ScopedTimer timer;
    auto result = sites(cursor.symbol, RelationKind::Declaration);
    auto defs = sites(cursor.symbol, RelationKind::Definition);
    result.insert(result.end(),
                  std::make_move_iterator(defs.begin()),
                  std::make_move_iterator(defs.end()));
    dedup_sites(result);
    drop_cursor_site(result, cursor.site);
    LOG_PERF("index_query",
             "kind=declaration path={} results={} elapsed_ms={:.2f}",
             cursor.site.path,
             result.size(),
             timer.ms_f());
    return result;
}

std::vector<Site> IndexQuery::references(const Cursor& cursor, bool include_declaration) const {
    ScopedTimer timer;
    auto result = sites(cursor.symbol, RelationKind::Reference);
    if(include_declaration) {
        for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
            auto extra = sites(cursor.symbol, kind);
            result.insert(result.end(),
                          std::make_move_iterator(extra.begin()),
                          std::make_move_iterator(extra.end()));
        }
        dedup_sites(result);
    }
    LOG_PERF("index_query",
             "kind=references path={} results={} elapsed_ms={:.2f}",
             cursor.site.path,
             result.size(),
             timer.ms_f());
    return result;
}

std::vector<Site> IndexQuery::target_sites(index::SymbolHash hash, RelationKind kind) const {
    std::vector<Site> result;
    for(auto target: targets(hash, kind)) {
        if(auto site = canonical_site(target)) {
            result.push_back(*site);
        }
    }
    return result;
}

std::vector<Site> IndexQuery::implementation(index::SymbolHash hash) const {
    auto info = symbol_info(hash);
    if(!info) {
        return {};
    }
    bool type_like = info->kind == SymbolKind::Class || info->kind == SymbolKind::Struct ||
                     info->kind == SymbolKind::Union;
    return target_sites(hash, type_like ? RelationKind::Derived : RelationKind::Implementation);
}

std::optional<llvm::StringRef>
    IndexQuery::source_text(const RowSource& source,
                            std::unique_ptr<llvm::MemoryBuffer>& storage) const {
    // Buffer-backed sources and non-ASCII blobs carry their text; an
    // open session served by its shard reads in buffer coordinates, so it
    // lands here too.
    if(source.kind == RowSource::Kind::SessionRows ||
       source.kind == RowSource::Kind::PreambleRows || !source.coords.text().empty()) {
        return source.coords.text();
    }
    return disk_text(source.path, *source.rows, storage);
}

std::optional<IndexQuery::Definition> IndexQuery::definition_text(index::SymbolHash hash) const {
    // Live sources first: buffer-true rows also know symbols the project
    // table has never seen (an unsaved definition), the preamble region
    // holds the buffer's own macros, and an overlay is the only source for
    // a header seen under the live context. The first in-bounds Definition
    // payload whose text is available wins.
    std::optional<Definition> found;
    for_each_relation(hash,
                      RelationKind::Definition,
                      Order::LiveFirst,
                      {},
                      [&](const RowSource& source, const index::Relation& relation) {
                          auto extent = std::bit_cast<LocalSourceRange>(relation.target_symbol);
                          if(extent.begin >= extent.end || extent.end > source.coords.size()) {
                              return true;
                          }
                          std::unique_ptr<llvm::MemoryBuffer> storage;
                          auto text = source_text(source, storage);
                          if(!text) {
                              return true;
                          }
                          found = Definition{
                              .extent = source.site(extent),
                              .text = std::string(text->substr(extent.begin, extent.length())),
                              .comment = feature::preceding_comment(*text, extent.begin),
                          };
                          return false;
                      });
    return found;
}

std::string IndexQuery::context_line(const Site& site) const {
    if(!site.coords.text().empty()) {
        return extract_line(site.coords.text(), site.range.begin);
    }
    if(!site.file.valid()) {
        return {};
    }
    auto it = workspace.shards.find(site.file);
    if(it == workspace.shards.end()) {
        return {};
    }
    std::unique_ptr<llvm::MemoryBuffer> storage;
    auto text = disk_text(site.path, it->second, storage);
    return text ? extract_line(*text, site.range.begin) : std::string{};
}

std::optional<IndexQuery::Located> IndexQuery::resolve(index::SymbolHash hash) const {
    auto info = symbol_info(hash);
    if(!info) {
        return std::nullopt;
    }
    auto site = canonical_site(hash);
    if(!site) {
        return std::nullopt;
    }
    return Located{.symbol = std::move(*info), .site = *site};
}

std::vector<IndexQuery::Located>
    IndexQuery::search(llvm::StringRef query,
                       std::size_t limit,
                       llvm::function_ref<bool(SymbolKind)> accept) const {
    ScopedTimer timer;

    // `ns::name` names a scope and a name: the name part matches like a
    // bare query and the scope part selects containers (`ns::` alone
    // lists a scope); a leading `::` pins the container exactly.
    llvm::StringRef pattern = query;
    std::optional<llvm::StringRef> qualifier;
    bool absolute = false;
    if(auto colons = query.rfind("::"); colons != llvm::StringRef::npos) {
        qualifier = query.take_front(colons);
        pattern = query.drop_front(colons + 2);
        absolute = qualifier->consume_front("::");
    }

    // Exact name, prefix, substring — ranked before the cut, so a weak
    // match never displaces the exact one behind an arbitrary table order.
    auto score = [&](llvm::StringRef name) -> int {
        if(pattern.empty()) {
            return 0;
        }
        auto at = name.find_insensitive(pattern);
        if(at == llvm::StringRef::npos) {
            return -1;
        }
        if(name.size() == pattern.size()) {
            return 0;
        }
        return at == 0 ? 1 : 2;
    };

    struct Candidate {
        int score;
        std::string name;
        index::SymbolHash hash;
    };

    std::vector<Candidate> candidates;
    llvm::DenseSet<index::SymbolHash> seen;
    // Matched and ranked by the displayed name, so a specialization
    // answers to `Box<int>` as well as `Box`.
    auto consider =
        [&](index::SymbolHash hash, llvm::StringRef name, llvm::StringRef args, SymbolKind kind) {
            if(!is_indexable_kind(kind) || name.empty() || (accept && !accept(kind)) ||
               !seen.insert(hash).second) {
                return;
            }
            llvm::SmallString<64> display(name);
            display += args;
            auto rank = score(display);
            if(rank >= 0) {
                candidates.push_back({.score = rank, .name = std::string(display), .hash = hash});
            }
        };

    // Only defined symbols are listed. The project table's flag is the
    // union over every indexed unit, so it spares the shard read that
    // would otherwise reject each declaration-only match; a session's
    // own rows cover just its main file, and its headers' definitions are
    // found through the preamble and PCH overlays by the site check.
    for(auto& [hash, symbol]: workspace.project_index.symbols) {
        if(index::has_flag(symbol.flags, index::SymbolFlags::HasDefinition)) {
            consider(hash, symbol.name, symbol.args, symbol.kind);
        }
    }
    visit_sessions([&](Fid path_id, const Session&) -> bool {
        sources.projections->projection(path_id)->index->iterate_symbols(
            [&](index::SymbolHash hash, const index::SymbolIdentity& symbol, llvm::StringRef) {
                consider(hash, symbol.name, symbol.args, symbol.kind);
                return true;
            });
        return true;
    });

    // The scope's components must appear in the container's, in order —
    // `inner::paint` finds `outer::inner::Widget::paint` — and match it
    // exactly when the query was absolute.
    auto in_scope = [&](index::SymbolHash hash) {
        if(!qualifier) {
            return true;
        }
        auto container = container_name(hash);
        llvm::SmallVector<llvm::StringRef> wanted;
        llvm::StringRef(*qualifier).split(wanted, "::", -1, false);
        llvm::SmallVector<llvm::StringRef> have;
        llvm::StringRef(container).split(have, "::", -1, false);
        if(absolute && wanted.size() != have.size()) {
            return false;
        }
        std::size_t next = 0;
        for(auto component: have) {
            if(next < wanted.size() && component.equals_insensitive(wanted[next])) {
                next += 1;
            } else if(absolute) {
                return false;
            }
        }
        return next == wanted.size();
    };

    // Ranked lazily through a heap: a broad query matches most of the
    // table, and the cut should cost its `limit` results, not a sort of
    // every match. A candidate outside the requested scope or without a
    // definition site cedes its slot to the next one, which a top-k cut
    // ahead of those checks could not.
    auto worse = [](const Candidate& lhs, const Candidate& rhs) {
        return std::tie(lhs.score, lhs.name, lhs.hash) > std::tie(rhs.score, rhs.name, rhs.hash);
    };
    std::ranges::make_heap(candidates, worse);

    std::vector<Located> results;
    while(!candidates.empty() && results.size() < limit) {
        std::ranges::pop_heap(candidates, worse);
        auto candidate = candidates.back();
        candidates.pop_back();
        if(!in_scope(candidate.hash)) {
            continue;
        }
        if(auto site = first_site(candidate.hash, RelationKind::Definition)) {
            results.push_back({.symbol = *symbol_info(candidate.hash), .site = *site});
        }
    }
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

std::vector<IndexQuery::Located> IndexQuery::locate(const SymbolLocator& locator) const {
    if(locator.symbol) {
        auto hash = *locator.symbol;
        auto info = symbol_info(hash);
        if(!info) {
            return {};
        }
        auto site = first_site(hash, RelationKind::Definition);
        if(!site) {
            return {};
        }
        return {
            {.symbol = std::move(*info), .site = *site}
        };
    }

    if(!locator.name.empty()) {
        std::string query_lower = locator.name.lower();
        // A qualified query ("ns::Foo") is matched against the qualified
        // name, a bare one against the symbol's own name.
        bool qualified = locator.name.contains("::");
        std::vector<Located> candidates;
        std::vector<Located> exact_matches;

        for(auto& [hash, symbol]: workspace.project_index.symbols) {
            if(symbol.name.empty() ||
               !index::has_flag(symbol.flags, index::SymbolFlags::HasDefinition)) {
                continue;
            }
            if(qualified ? llvm::StringRef(qualified_name(hash)).lower().find(query_lower) ==
                               std::string::npos
                         : !name_contains(symbol, query_lower)) {
                continue;
            }
            auto name = qualified ? qualified_name(hash) : symbol.name + symbol.args;
            auto site = first_site(hash, RelationKind::Definition);
            if(!site) {
                continue;
            }
            if(!locator.path.empty()) {
                llvm::StringRef wanted = locator.path;
                bool basename_only = wanted.find_last_of("/\\") == llvm::StringRef::npos;
                if(basename_only) {
                    if(llvm::sys::path::filename(site->path) != wanted)
                        continue;
                } else if(!site->path.ends_with(wanted)) {
                    continue;
                }
            }

            bool is_exact = llvm::StringRef(name).lower() == query_lower ||
                            llvm::StringRef(name).ends_with(("::" + locator.name).str());
            Located located{.symbol = to_ref(hash, symbol), .site = *site};
            if(is_exact)
                exact_matches.push_back(std::move(located));
            else
                candidates.push_back(std::move(located));
        }

        if(!exact_matches.empty())
            return exact_matches;
        return candidates;
    }

    if(!locator.path.empty() && locator.line.has_value()) {
        auto path_id = workspace.file_table.find(locator.path);
        if(!path_id) {
            return {};
        }
        // Stale rows describe text that no longer exists: resolving the
        // requested line against them would name the wrong symbol.
        auto serving = serving_source(*path_id);
        if(!serving) {
            return {};
        }
        auto target_line = static_cast<protocol::uinteger>(*locator.line - 1);
        auto path = workspace.file_table.resolve(*path_id);
        for(auto& [hash, symbol]: workspace.project_index.symbols) {
            if(!symbol.reference_files.contains(path_id->raw))
                continue;
            std::optional<Located> found;
            serving.rows->lookup(hash, RelationKind::Definition, [&](const index::Relation& r) {
                auto position = serving.coords.to_position(r.range.begin);
                if(position && position->line == target_line) {
                    found = Located{
                        .symbol = to_ref(hash, symbol),
                        .site = {.file = *path_id,
                                 .path = path,
                                 .range = r.range,
                                 .coords = serving.coords},
                    };
                    return false;
                }
                return true;
            });
            if(found)
                return {std::move(*found)};
        }
        return {};
    }

    return {};
}

std::vector<IndexQuery::Located> IndexQuery::definitions_in(Fid file) const {
    auto serving = serving_source(file);
    if(!serving) {
        return {};
    }
    auto path = workspace.file_table.resolve(file);
    std::vector<Located> result;
    for(auto& [hash, symbol]: workspace.project_index.symbols) {
        if(symbol.name.empty() || !symbol.reference_files.contains(file.raw)) {
            continue;
        }
        serving.rows->lookup(hash, RelationKind::Definition, [&](const index::Relation& r) {
            result.push_back({
                .symbol = to_ref(hash, symbol),
                .site = {.file = file, .path = path, .range = r.range, .coords = serving.coords},
            });
            return true;
        });
    }
    return result;
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

    auto version_of = [&](VersionID fv) -> const FileTable::FileVersion* {
        return workspace.file_table.knows_version(fv) ? &workspace.file_table.version(fv) : nullptr;
    };
    auto is_document = [&](VersionID fv) {
        const auto* version = version_of(fv);
        return version && version->fid == session.path_id && version->content_hash == generation;
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
            document_nodes[i] = is_document(VersionID{node.file});
        }
        for(const auto& node: manifest.nodes) {
            if(node.parent == ~0u ? !root_is_document : !document_nodes[node.parent]) {
                continue;
            }
            const auto* target = version_of(VersionID{node.file});
            if(!target) {
                continue;
            }
            edges.push_back({
                .line = node.line,
                .target = std::string(workspace.file_table.resolve(target->fid)),
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

}  // namespace clice
