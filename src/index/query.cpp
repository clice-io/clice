#include "index/query.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <string>
#include <tuple>
#include <vector>

#include "index/search_index.h"
#include "support/logging.h"
#include "support/timer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice::index {

namespace {

Coordinates shard_coordinates(const Shard& shard) {
    return {shard.content(), shard.content_size(), shard.line_starts()};
}

LocalSourceRange to_local(const Occurrence& occurrence) {
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
                                         const Shard& shard,
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

}  // namespace

bool DiskGate::withhold(Fid file) const {
    auto [it, inserted] = verdicts.try_emplace(file, false);
    if(inserted) {
        auto shard = index.shards.find(file);
        if(shard != index.shards.end()) {
            auto disk = files.current(file);
            it->second = !disk || !shard->second.matches_content(disk->size, disk->hash);
        }
    }
    return it->second;
}

llvm::SmallVector<Fid> DiskGate::withheld() const {
    llvm::SmallVector<Fid> result;
    for(auto& [file, stale]: verdicts) {
        if(stale) {
            result.push_back(file);
        }
    }
    return result;
}

IndexQuery::IndexQuery(const ProjectIndex& index,
                       const FileTable& files,
                       const FreshnessGate* gate,
                       const LiveSources* live) :
    index(index), files(files), gate(gate), live(live) {}

std::optional<RowSource> IndexQuery::serving(Fid file) const {
    if(live && live->is_open(file)) {
        return live->claim(file);
    }
    if(gate && gate->withhold(file)) {
        return std::nullopt;
    }
    auto it = index.shards.find(file);
    if(it == index.shards.end()) {
        return std::nullopt;
    }
    return RowSource{.kind = RowSource::Kind::Shard,
                     .file = file,
                     .path = files.resolve(file),
                     .rows = &it->second,
                     .coords = shard_coordinates(it->second)};
}

const Shard* IndexQuery::shard_matching(Fid file, llvm::StringRef text) const {
    auto it = index.shards.find(file);
    if(it == index.shards.end() || !it->second.matches_content(text)) {
        return nullptr;
    }
    return &it->second;
}

std::shared_ptr<TUIndex> IndexQuery::preamble_blob(Fid file) const {
    return live ? live->preamble_blob(file) : nullptr;
}

void IndexQuery::visit_overlay_files(const TUIndex& state,
                                     llvm::function_ref<bool(const RowSource&)> visitor) const {
    auto main_id = state.path_count() - 1;
    for(std::uint32_t i = 0; i < state.section_count(); i += 1) {
        auto local_id = state.section_path(i);
        if(local_id == main_id) {
            continue;
        }
        auto path = state.path(local_id);
        Fid file;
        if(auto known = files.find(path)) {
            if(live->is_open(*known) || (gate && gate->withhold(*known))) {
                continue;
            }
            file = *known;
            path = files.resolve(file);
        }
        if(live->excluded(path)) {
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

void IndexQuery::for_each_relation(SymbolHash hash,
                                   RelationKind kind,
                                   Order order,
                                   SourceMask mask,
                                   RelationVisitor visitor) const {
    bool stopped = false;
    auto emit = [&](const RowSource& source) {
        source.rows->lookup(hash, kind, [&](const Relation& relation) {
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
        auto it = index.symbols.find(hash);
        if(it == index.symbols.end()) {
            return true;
        }
        for(auto file_id: it->second.reference_files) {
            auto source = serving(Fid{file_id});
            if(!source || source->kind != RowSource::Kind::Shard) {
                continue;
            }
            if(!emit(*source)) {
                return false;
            }
        }
        return true;
    };
    auto sessions = [&] {
        if(!mask.session || !live) {
            return true;
        }
        live->each_session(emit);
        return !stopped;
    };
    auto preambles = [&] {
        if(!mask.preamble || !live) {
            return true;
        }
        live->each_preamble(emit);
        return !stopped;
    };
    auto overlays = [&] {
        if(!mask.overlay || !live) {
            return true;
        }
        live->each_overlay([&](const TUIndex& state) {
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
    auto source = serving(file);
    if(!source) {
        return std::nullopt;
    }
    std::optional<Cursor> cursor;
    auto hit = [&](const Shard& rows) {
        // Several symbols can share the name span: a module imported
        // through a macro sits under the macro's own occurrence. The
        // name spells what the macro expanded to; the macro itself is
        // reached at its definition.
        llvm::SmallVector<Occurrence, 2> candidates;
        rows.lookup(offset, [&](const Occurrence& occurrence) {
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
        auto site =
            RowSource{.file = file, .path = source->path, .rows = &rows, .coords = source->coords}
                .site(to_local(chosen));
        if(!site) {
            return false;
        }
        cursor = Cursor{.symbol = chosen.target, .site = *site};
        return true;
    };
    if(hit(*source->rows)) {
        return cursor;
    }
    if(source->kind != RowSource::Kind::SessionRows) {
        return std::nullopt;
    }
    // The preamble region is compiled into the PCH and invisible to the
    // per-edit index; its occurrences (macro definitions and references
    // before the bound) live in the PCH's overlay, in the same buffer
    // coordinates — served only under the main-entry gate (preamble
    // drift, shared-PCH identity).
    live->each_preamble([&](const RowSource& preamble) {
        if(preamble.file != file) {
            return true;
        }
        hit(*preamble.rows);
        return false;
    });
    return cursor;
}

std::optional<IndexQuery::Cursor> IndexQuery::symbol_at(Fid file,
                                                        std::uint32_t line,
                                                        std::uint32_t utf16_column) const {
    auto source = serving(file);
    if(!source) {
        return std::nullopt;
    }
    auto offset = source->coords.offset(line, utf16_column);
    if(!offset) {
        return std::nullopt;
    }
    return symbol_at(file, *offset);
}

std::optional<SymbolRef> IndexQuery::symbol_info(SymbolHash hash) const {
    std::optional<SymbolRef> found;
    auto adopt = [&](const SymbolIdentity& identity) {
        found = SymbolRef::from(hash, identity);
    };

    // Open sessions first: they hold every symbol of their unsaved buffers.
    if(live) {
        live->each_session_index([&](const TUIndex& state) {
            if(auto identity = state.find_symbol(hash)) {
                adopt(*identity);
            }
            return !found;
        });
        if(found) {
            return found;
        }
    }

    auto it = index.symbols.find(hash);
    if(it != index.symbols.end()) {
        return SymbolRef::from(hash, it->second.identity());
    }

    // A symbol that exists only under an open buffer's context (or in
    // headers no disk TU has been indexed with) is in no disk table.
    if(live) {
        live->each_overlay([&](const TUIndex& state) {
            if(auto identity = state.find_symbol(hash)) {
                adopt(*identity);
            }
            return !found;
        });
        if(found) {
            return found;
        }
    }

    // Each shard stores exactly the local symbols its occurrences
    // reference, so a TU-local name is in the shard that produced it.
    for(auto& [path_id, shard]: index.shards) {
        if(auto identity = shard.find_symbol(hash)) {
            adopt(*identity);
            return found;
        }
    }
    return std::nullopt;
}

llvm::SmallVector<SymbolRef, 4> IndexQuery::container_chain(SymbolHash hash) const {
    llvm::SmallVector<SymbolRef, 4> chain;
    auto symbol = symbol_info(hash);
    if(!symbol) {
        return chain;
    }
    // A parent chain follows declaration contexts, so it is acyclic as
    // built; the guard keeps a corrupted parent column from spinning.
    llvm::DenseSet<SymbolHash> visited{hash};
    for(auto parent = symbol->parent; parent != 0 && visited.insert(parent).second;) {
        auto scope = symbol_info(parent);
        if(!scope) {
            break;
        }
        parent = scope->parent;
        if(!has_flag(scope->flags, SymbolFlags::InlineNamespace)) {
            chain.push_back(std::move(*scope));
        }
    }
    std::ranges::reverse(chain);
    return chain;
}

std::string IndexQuery::container_name(SymbolHash hash) const {
    std::string result;
    for(auto& scope: container_chain(hash)) {
        if(!result.empty()) {
            result += "::";
        }
        result += scope.display_name();
    }
    return result;
}

std::string IndexQuery::qualified_name(SymbolHash hash) const {
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

std::vector<Site> IndexQuery::sites(SymbolHash hash, RelationKind kind) const {
    std::vector<Site> result;
    for_each_relation(hash,
                      kind,
                      Order::DiskFirst,
                      {},
                      [&](const RowSource& source, const Relation& relation) {
                          if(auto site = source.site(relation.range)) {
                              result.push_back(*site);
                          }
                          return true;
                      });
    // Same-kind rows can share one anchor: a macro body using an argument
    // twice spells both references at the one written token.
    dedup_sites(result);
    return result;
}

std::optional<Site> IndexQuery::first_site(SymbolHash hash, RelationKind kind) const {
    std::optional<Site> result;
    for_each_relation(hash,
                      kind,
                      Order::LiveFirst,
                      {},
                      [&](const RowSource& source, const Relation& relation) {
                          result = source.site(relation.range);
                          return !result;
                      });
    return result;
}

std::optional<Site> IndexQuery::canonical_site(SymbolHash hash) const {
    if(auto site = first_site(hash, RelationKind::Definition)) {
        return site;
    }
    // A declaration stands in only for a symbol nothing defines: a
    // definition withheld as stale stays unavailable, as documented. An
    // open session's identity may know only the declaration; the project
    // row remembers the definition.
    auto info = symbol_info(hash);
    if(!info) {
        return std::nullopt;
    }
    bool defined = has_flag(info->flags, SymbolFlags::HasDefinition);
    if(auto row = index.symbols.find(hash); row != index.symbols.end()) {
        defined = defined || has_flag(row->second.flags, SymbolFlags::HasDefinition);
    }
    if(defined) {
        return std::nullopt;
    }
    return first_site(hash, RelationKind::Declaration);
}

std::vector<IndexQuery::Edge> IndexQuery::edges(SymbolHash hash, RelationKind kind) const {
    // The main-file preamble entry cannot contribute: the preamble region
    // holds only preprocessor directives, never call or type relations.
    llvm::DenseMap<SymbolHash, std::vector<Site>> by_target;
    for_each_relation(hash,
                      kind,
                      Order::DiskFirst,
                      {.preamble = false},
                      [&](const RowSource& source, const Relation& relation) {
                          if(auto site = source.site(relation.range)) {
                              by_target[relation.target_symbol].push_back(*site);
                          }
                          return true;
                      });
    std::vector<Edge> result;
    result.reserve(by_target.size());
    for(auto& [target, sites]: by_target) {
        auto located = resolve(target);
        if(!located) {
            continue;
        }
        // A row present in both a shard and an overlay lands twice;
        // hierarchy items must not repeat call sites.
        dedup_sites(sites);
        result.push_back({.symbol = std::move(*located), .sites = std::move(sites)});
    }
    return result;
}

std::vector<IndexQuery::Located> IndexQuery::located_targets(SymbolHash hash,
                                                             RelationKind kind) const {
    std::vector<Located> result;
    for(auto target: targets(hash, kind)) {
        if(auto located = resolve(target)) {
            result.push_back(std::move(*located));
        }
    }
    return result;
}

IndexQuery::CallGraph IndexQuery::call_graph(SymbolHash root, CallGraphOptions options) const {
    CallGraph graph;
    if(options.callers) {
        graph.callers = edges(root, RelationKind::Caller);
    }
    if(options.callees) {
        graph.callees = edges(root, RelationKind::Callee);
    }
    return graph;
}

IndexQuery::TypeHierarchy IndexQuery::type_hierarchy(SymbolHash root,
                                                     TypeHierarchyOptions options) const {
    TypeHierarchy hierarchy;
    if(options.supertypes) {
        hierarchy.supertypes = located_targets(root, RelationKind::Base);
    }
    if(options.subtypes) {
        hierarchy.subtypes = located_targets(root, RelationKind::Derived);
    }
    return hierarchy;
}

llvm::SmallVector<SymbolHash> IndexQuery::targets(SymbolHash hash, RelationKind kind) const {
    llvm::SmallVector<SymbolHash> result;
    llvm::DenseSet<SymbolHash> seen;
    for_each_relation(hash,
                      kind,
                      Order::DiskFirst,
                      {.preamble = false},
                      [&](const RowSource&, const Relation& relation) {
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

std::vector<Site> IndexQuery::target_sites(SymbolHash hash, RelationKind kind) const {
    std::vector<Site> result;
    for(auto& located: located_targets(hash, kind)) {
        result.push_back(located.site);
    }
    return result;
}

std::vector<Site> IndexQuery::implementation(SymbolHash hash) const {
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

std::optional<IndexQuery::Definition> IndexQuery::definition_text(SymbolHash hash) const {
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
                      [&](const RowSource& source, const Relation& relation) {
                          auto extent = std::bit_cast<LocalSourceRange>(relation.target_symbol);
                          if(extent.begin >= extent.end || extent.end > source.coords.size()) {
                              return true;
                          }
                          auto site = source.site(extent);
                          if(!site) {
                              return true;
                          }
                          std::unique_ptr<llvm::MemoryBuffer> storage;
                          auto text = source_text(source, storage);
                          if(!text) {
                              return true;
                          }
                          found = Definition{
                              .extent = *site,
                              .text = std::string(text->substr(extent.begin, extent.length())),
                              .comment = feature::preceding_comment(*text, extent.begin),
                          };
                          return false;
                      });
    return found;
}

std::string IndexQuery::context_line(const Site& site) const {
    if(!site.file.valid()) {
        return {};
    }
    if(auto source = serving(site.file); source && !source->coords.text().empty()) {
        return extract_line(source->coords.text(), site.range.begin);
    }
    auto it = index.shards.find(site.file);
    if(it == index.shards.end()) {
        return {};
    }
    std::unique_ptr<llvm::MemoryBuffer> storage;
    auto text = disk_text(site.path, it->second, storage);
    return text ? extract_line(*text, site.range.begin) : std::string{};
}

std::optional<IndexQuery::Located> IndexQuery::resolve(SymbolHash hash) const {
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

IndexQuery::RankedHits IndexQuery::ranked_search(const SymbolQuery& query,
                                                 std::size_t limit) const {
    std::vector<Ranked> hits;
    llvm::DenseSet<SymbolHash> seen;
    auto indexed = index.search_index.search(query, limit);
    // A damaged index answers incompletely: until its rebuild the whole
    // table is judged row by row instead.
    bool scan_table = index.search_index.damaged();
    bool exhausted = scan_table || indexed.exhausted;
    if(!scan_table) {
        for(auto& hit: indexed.hits) {
            // A row the table changed since the index was built is read
            // from the table below, not from the index's stale copy.
            if(index.search_pending.contains(hit.hash)) {
                continue;
            }
            auto info = symbol_info(hit.hash);
            if(info && seen.insert(hit.hash).second) {
                hits.push_back({hit.rank, std::move(*info)});
            }
        }
    }

    // What the index does not file — the symbols merged since it was
    // built, and the open sessions' — is judged row by row against the
    // same query.
    NameRanker ranker(query);
    auto consider = [&](SymbolHash hash,
                        const SymbolIdentity& identity,
                        llvm::StringRef path,
                        std::uint32_t reference_files) {
        if(!is_searchable_kind(identity.kind) || identity.name.empty() || seen.contains(hash)) {
            return;
        }
        if(!query.kinds.empty() && !llvm::is_contained(query.kinds, identity.kind)) {
            return;
        }
        // A function's locals (parameters, local variables and classes)
        // are no one's search target.
        auto containers = container_chain(hash);
        if(llvm::any_of(containers, [](const SymbolRef& container) {
               return container.kind == SymbolKind::Function ||
                      container.kind == SymbolKind::Method ||
                      container.kind == SymbolKind::Operator;
           })) {
            return;
        }
        if(!query.paths.empty() && llvm::none_of(query.paths, [&](const std::string& wanted) {
               return !path.empty() && path_matches(wanted, path);
           })) {
            return;
        }
        if(query.absolute || !query.scope.empty() || query.mode == SymbolQuery::Mode::Members) {
            llvm::SmallVector<ScopeEntry, 4> chain;
            for(auto& container: containers) {
                chain.push_back({.name = container.name, .args = container.args});
            }
            if(!in_scope(query, chain)) {
                return;
            }
        }
        auto quality =
            symbol_quality(identity.name, identity.kind, identity.flags, reference_files);
        if(auto rank = ranker.rank(identity.name, identity.args, quality, /*lenient=*/true)) {
            seen.insert(hash);
            hits.push_back({*rank, SymbolRef::from(hash, identity)});
        }
    };
    auto references_of = [&](SymbolHash hash) -> std::uint32_t {
        auto it = index.symbols.find(hash);
        if(it == index.symbols.end()) {
            return 0;
        }
        return static_cast<std::uint32_t>(it->second.reference_files.cardinality());
    };
    auto consider_row = [&](SymbolHash hash, const Symbol& symbol) {
        llvm::StringRef path;
        if(symbol.file != no_file) {
            path = files.resolve(Fid{symbol.file});
        }
        consider(hash, symbol.identity(), path, references_of(hash));
    };
    if(scan_table) {
        for(auto& [hash, symbol]: index.symbols) {
            consider_row(hash, symbol);
        }
    } else {
        for(auto hash: index.search_pending) {
            auto it = index.symbols.find(hash);
            if(it != index.symbols.end()) {
                consider_row(hash, it->second);
            }
        }
    }
    if(live) {
        live->each_session_index([&](const TUIndex& state) {
            state.iterate_symbols(
                [&](SymbolHash hash, const SymbolIdentity& identity, llvm::StringRef) {
                    llvm::StringRef path;
                    if(identity.file != no_file) {
                        path = state.path(identity.file);
                    }
                    consider(hash, identity, path, references_of(hash));
                    return true;
                });
            return true;
        });
    }

    std::ranges::sort(hits, [](const Ranked& lhs, const Ranked& rhs) {
        return ranks_after(rhs.rank,
                           rhs.symbol.name,
                           rhs.symbol.args,
                           rhs.symbol.hash,
                           lhs.rank,
                           lhs.symbol.name,
                           lhs.symbol.args,
                           lhs.symbol.hash);
    });
    if(hits.size() > limit) {
        hits.resize(limit);
        exhausted = false;
    }
    return {.hits = std::move(hits), .exhausted = exhausted};
}

std::vector<IndexQuery::Located> IndexQuery::search(const SymbolQuery& query,
                                                    std::size_t limit) const {
    ScopedTimer timer;
    if(limit == 0 || !query.by_pattern()) {
        return {};
    }
    // A symbol no source places cedes its slot to the next one: the
    // ranking is fetched wider until the cut is filled or exhausted.
    std::vector<Located> results;
    for(std::size_t fetch = limit * 2;; fetch *= 4) {
        auto ranked = ranked_search(query, fetch);
        results.clear();
        for(auto& hit: ranked.hits) {
            if(results.size() == limit) {
                break;
            }
            if(auto site = canonical_site(hit.symbol.hash)) {
                results.push_back({.symbol = std::move(hit.symbol), .site = *site});
            }
        }
        if(results.size() == limit || ranked.exhausted) {
            break;
        }
    }
    LOG_PERF("index_query",
             "kind=search pattern_len={} results={} elapsed_ms={:.2f}",
             query.pattern.size(),
             results.size(),
             timer.ms_f());
    return results;
}

std::vector<IndexQuery::Located> IndexQuery::locate(const SymbolQuery& query) const {
    if(query.handle) {
        if(auto located = resolve(*query.handle)) {
            return {std::move(*located)};
        }
        return {};
    }

    if(query.position) {
        auto& place = *query.position;
        auto path_id = files.find(place.path);
        if(!path_id) {
            return {};
        }
        // Stale rows describe text that no longer exists: resolving the
        // requested place against them would name the wrong symbol.
        auto source = serving(*path_id);
        if(!source) {
            return {};
        }
        auto line = static_cast<std::uint32_t>(place.line - 1);
        auto bounds = source->coords.line_bounds(line);
        if(!bounds) {
            return {};
        }
        if(place.column) {
            // The column counts bytes: the line's start plus the column,
            // bounded by the line's own length.
            auto offset = bounds->begin + static_cast<std::uint32_t>(*place.column - 1);
            if(offset > bounds->end) {
                return {};
            }
            auto cursor = symbol_at(*path_id, offset);
            if(!cursor) {
                return {};
            }
            if(auto located = resolve(cursor->symbol)) {
                return {std::move(*located)};
            }
            // A symbol of the file's own (a static function, a local) has
            // no row in the global table to fan out from: its sites are
            // in the serving source itself.
            auto info = symbol_info(cursor->symbol);
            if(!info) {
                return {};
            }
            std::optional<Site> site;
            for(auto kind: {RelationKind::Definition, RelationKind::Declaration}) {
                source->rows->lookup(cursor->symbol, kind, [&](const Relation& relation) {
                    site = source->site(relation.range);
                    return !site;
                });
                if(site) {
                    return {
                        Located{.symbol = std::move(*info), .site = *site}
                    };
                }
            }
            return {};
        }
        // The definitions on the line, the file's own symbols included:
        // the serving source holds every row, whichever table names it.
        std::vector<Located> defined;
        llvm::DenseSet<SymbolHash> seen;
        source->rows->for_each_relation([&](SymbolHash hash, const Relation& r) {
            if(r.kind != RelationKind::Definition || !bounds->contains(r.range.begin) ||
               !seen.insert(hash).second) {
                return true;
            }
            auto site = source->site(r.range);
            if(!site) {
                return true;
            }
            if(auto info = symbol_info(hash)) {
                defined.push_back({.symbol = std::move(*info), .site = *site});
            }
            return true;
        });
        return defined;
    }

    auto ranked = ranked_search(query, 50).hits;
    // Spelling the name exactly settles it; otherwise every match stands.
    bool any_exact = llvm::any_of(ranked, [](const Ranked& hit) { return hit.rank.tier <= 1; });
    std::vector<Located> results;
    for(auto& hit: ranked) {
        if(any_exact && hit.rank.tier > 1) {
            continue;
        }
        if(auto site = canonical_site(hit.symbol.hash)) {
            results.push_back({.symbol = std::move(hit.symbol), .site = *site});
        }
    }
    return results;
}

std::vector<IndexQuery::Located> IndexQuery::definitions_in(Fid file) const {
    auto source = serving(file);
    if(!source) {
        return {};
    }
    std::vector<Located> result;
    for(auto& [hash, symbol]: index.symbols) {
        if(symbol.name.empty() || !symbol.reference_files.contains(file.raw)) {
            continue;
        }
        source->rows->lookup(hash, RelationKind::Definition, [&](const Relation& r) {
            if(auto site = source->site(r.range)) {
                result.push_back(
                    {.symbol = SymbolRef::from(hash, symbol.identity()), .site = *site});
            }
            return true;
        });
    }
    return result;
}

std::vector<IncludeEdge> IndexQuery::include_edges(Fid file) const {
    // Every consumer projects the edges onto content the serving shard
    // matches, so a manifest contributes only where the version it entered
    // for this document carries that same content generation — a TU that
    // indexed an older revision would place its lines in text that moved.
    auto shard_it = index.shards.find(file);
    if(shard_it == index.shards.end()) {
        return {};
    }
    auto generation = shard_it->second.content_hash();

    auto version_of = [&](VersionID fv) -> const FileTable::FileVersion* {
        return files.knows_version(fv) ? &files.version(fv) : nullptr;
    };
    auto is_document = [&](VersionID fv) {
        const auto* version = version_of(fv);
        return version && version->fid == file && version->content_hash == generation;
    };

    // A directive line of the document is a node whose parent node entered
    // this file: the TU root (parent == no_node) when the document is the TU
    // itself, or any node of the document's own file version otherwise
    // (directives inside included headers hang off the node of the file
    // that contains them).
    std::vector<IncludeEdge> edges;
    auto append = [&](const TUManifest& manifest) {
        bool root_is_document = is_document(manifest.tu_fv);
        llvm::SmallVector<bool> document_nodes(manifest.nodes.size());
        for(auto [i, node]: llvm::enumerate(manifest.nodes)) {
            document_nodes[i] = is_document(VersionID{node.file});
        }
        for(const auto& node: manifest.nodes) {
            if(node.parent == no_node ? !root_is_document : !document_nodes[node.parent]) {
                continue;
            }
            const auto* target = version_of(VersionID{node.file});
            if(!target) {
                continue;
            }
            edges.push_back({
                .line = node.line,
                .target = std::string(files.resolve(target->fid)),
            });
        }
    };

    // The document's own manifest is its own context and answers alone; a
    // header reached only through source TUs has none, and its directives
    // live in the contributing TUs' manifests instead. The generation gate
    // above dedups divergent revisions; agreeing TUs collapse in the
    // projection's dedup.
    if(auto manifest_it = index.manifests.find(file); manifest_it != index.manifests.end()) {
        append(manifest_it->second);
        return edges;
    }
    if(auto contribution_it = index.contributions.find(file);
       contribution_it != index.contributions.end()) {
        for(auto tu: llvm::make_first_range(contribution_it->second)) {
            if(auto manifest_it = index.manifests.find(tu); manifest_it != index.manifests.end()) {
                append(manifest_it->second);
            }
        }
    }
    return edges;
}

}  // namespace clice::index
