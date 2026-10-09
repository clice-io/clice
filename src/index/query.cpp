module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.query;
import :index.search_index;
import :support.logging;
import :support.text;
import :support.timer;
import :vfs.file_system;

namespace clice::index {

namespace {

std::string extract_line(llvm::StringRef content, std::uint32_t offset) {
    if(content.empty() || offset >= content.size())
        return {};
    // StringRef::rfind looks strictly before its position.
    std::size_t line_start = 0;
    if(auto pos = content.rfind('\n', offset); pos != llvm::StringRef::npos) {
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
    auto buffer = vfs::read(path);
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

/// The whole declaration a declaring row names, when the row carries its
/// extent in this source's text.
std::optional<Site> extent_of(const RowSource& source, const Relation& relation) {
    assert(RelationKind(relation.kind).isDeclOrDef());
    auto extent = std::bit_cast<LocalSourceRange>(relation.target_symbol);
    if(extent.begin >= extent.end) {
        return std::nullopt;
    }
    return source.site(extent);
}

/// Drop the cursor's own site from an answer set — standing on a
/// declaration or definition navigates to the other sites — unless it is
/// the only site the symbol has (an inline definition, nowhere else to go).
void drop_cursor_site(std::vector<Site>& sites, const Site& cursor) {
    if(sites.size() > 1) {
        std::erase_if(sites, [&](const Site& site) { return covers(site, cursor); });
    }
}

/// `parent`, the parent of `hash`, and the containers above it, outermost
/// first, transparent ones skipped (see IndexQuery::container_chain), each
/// resolved through `lookup`.
llvm::SmallVector<SymbolRef, 4>
    scope_chain(SymbolHash hash,
                SymbolHash parent,
                llvm::function_ref<std::optional<SymbolRef>(SymbolHash)> lookup) {
    llvm::SmallVector<SymbolRef, 4> chain;
    // A parent chain follows declaration contexts, so it is acyclic as
    // built; the guard keeps a corrupted parent column from spinning.
    llvm::DenseSet<SymbolHash> visited{hash};
    while(parent != 0 && visited.insert(parent).second) {
        auto scope = lookup(parent);
        if(!scope) {
            break;
        }
        parent = scope->parent;
        if(!transparent_scope(scope->flags)) {
            chain.push_back(std::move(*scope));
        }
    }
    std::ranges::reverse(chain);
    return chain;
}

}  // namespace

bool covers(const Site& row, const Site& cursor) {
    return row.path == cursor.path && row.range.contains(cursor.range);
}

void dedup_sites(std::vector<Site>& sites) {
    std::ranges::sort(sites, [](const Site& lhs, const Site& rhs) {
        return site_key(lhs) < site_key(rhs);
    });
    auto dup = std::ranges::unique(sites, [](const Site& lhs, const Site& rhs) {
        return site_key(lhs) == site_key(rhs);
    });
    sites.erase(dup.begin(), dup.end());
}

std::optional<Named> named_at(const Shard& rows,
                              std::uint32_t offset,
                              llvm::function_ref<bool(SymbolHash)> is_macro) {
    llvm::SmallVector<Occurrence, 2> candidates;
    rows.lookup(offset, [&](const Occurrence& occurrence) {
        if(!candidates.empty() && !(candidates.front().range == occurrence.range)) {
            return false;
        }
        candidates.push_back(occurrence);
        return true;
    });
    if(candidates.empty()) {
        return std::nullopt;
    }
    Named named{.range = candidates.front().range};
    for(auto& candidate: candidates) {
        if(!is_macro(candidate.target)) {
            named.symbols.push_back(candidate.target);
        }
    }
    if(named.symbols.empty()) {
        named.symbols.push_back(candidates.front().target);
    }
    return named;
}

std::vector<Highlight> highlights(llvm::ArrayRef<const Shard*> rows,
                                  llvm::ArrayRef<SymbolHash> symbols) {
    constexpr std::pair<RelationKind::Kind, HighlightKind> kinds[] = {
        {RelationKind::Declaration,   HighlightKind::Text },
        {RelationKind::Definition,    HighlightKind::Text },
        {RelationKind::Reference,     HighlightKind::Read },
        {RelationKind::WeakReference, HighlightKind::Read },
        {RelationKind::Write,         HighlightKind::Write},
    };
    std::vector<Highlight> result;
    for(auto* shard: rows) {
        for(auto symbol: symbols) {
            for(auto [relation, kind]: kinds) {
                shard->lookup(symbol, relation, [&](const Relation& row) {
                    result.push_back({.range = row.range, .kind = kind});
                    return true;
                });
            }
        }
    }
    // The strongest kind of a range sorts first and survives the dedup.
    std::ranges::sort(result, [](const Highlight& lhs, const Highlight& rhs) {
        return std::tuple(lhs.range.begin, lhs.range.end, rhs.kind) <
               std::tuple(rhs.range.begin, rhs.range.end, lhs.kind);
    });
    auto duplicates = std::ranges::unique(result, {}, &Highlight::range);
    result.erase(duplicates.begin(), duplicates.end());
    return result;
}

bool FreshnessGate::stale(Fid file, std::uint64_t content_hash) const {
    if(!options.withhold) {
        return false;
    }
    if(options.check_disk && checked.insert(file).second) {
        files.current(file);
    }
    if(!files.seen_missing(file)) {
        auto seen = files.seen_hash(file);
        if(!seen || *seen == content_hash) {
            return false;
        }
    }
    withheld_files.insert(file);
    return true;
}

IndexQuery::IndexQuery(const ProjectIndex& index,
                       FileTable& files,
                       const FreshnessGate* gate,
                       const LiveSources* live) :
    index(index), files(files), gate(gate), live(live) {}

std::optional<RowSource> IndexQuery::serving(Fid file) const {
    if(live && live->is_open(file)) {
        return live->claim(file);
    }
    auto* shard = index.shard(file);
    if(!shard || (gate && gate->stale(file, shard->content_hash()))) {
        return std::nullopt;
    }
    return RowSource{.kind = RowSource::Kind::Shard,
                     .file = file,
                     .path = files.display(file),
                     .rows = shard,
                     .positions = shard->positions()};
}

const Shard* IndexQuery::shard_matching(Fid file, llvm::StringRef text) const {
    auto* shard = index.shard(file);
    return shard && shard->matches_content(text) ? shard : nullptr;
}

std::shared_ptr<TUIndex> IndexQuery::preamble_blob(Fid file) const {
    return live ? live->preamble_blob(file) : nullptr;
}

bool IndexQuery::visit_overlay_rows(const TUIndex& state,
                                    SymbolHash hash,
                                    RelationKind kind,
                                    RelationVisitor visitor) const {
    auto main_id = state.path_count() - 1;
    bool stopped = false;
    for(std::uint32_t i = 0; i < state.section_count() && !stopped; i += 1) {
        auto local_id = state.section_path(i);
        if(local_id == main_id) {
            continue;
        }
        auto& shard = state.section_shard(i);
        std::optional<RowSource> source;
        shard.lookup(hash, kind, [&](const Relation& relation) {
            // An overlay spans hundreds of headers and few hold rows of any
            // one symbol: resolving every entry's file up front dominated
            // each lookup.
            if(!source) {
                auto file = files.intern(Spelling::absolute(state.path(local_id)));
                if(live->is_open(file) || (gate && gate->stale(file, shard.content_hash()))) {
                    return false;
                }
                source = RowSource{.kind = RowSource::Kind::Overlay,
                                   .file = file,
                                   .path = files.display(file),
                                   .rows = &shard,
                                   .positions = shard.positions()};
            }
            stopped = !visitor(*source, relation);
            return !stopped;
        });
    }
    return !stopped;
}

void IndexQuery::each_disk_file(SymbolHash hash,
                                Fid anchor,
                                llvm::function_ref<void(Fid)> visit) const {
    if(index.identity_of(hash)) {
        index.each_reference_file(hash, visit);
        return;
    }
    auto* shard = anchor.valid() ? index.shard(anchor) : nullptr;
    auto identity = shard ? shard->find_symbol(hash) : std::nullopt;
    if(!identity) {
        return;
    }
    if(identity->scope == SymbolScope::FileLocal) {
        visit(anchor);
        return;
    }
    index.each_fanout_file(hash, anchor, files, visit);
}

void IndexQuery::for_each_relation(SymbolHash hash,
                                   Fid anchor,
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
        bool completed = true;
        each_disk_file(hash, anchor, [&](Fid file) {
            if(!completed) {
                return;
            }
            auto source = serving(file);
            if(!source || source->kind != RowSource::Kind::Shard) {
                return;
            }
            completed = emit(*source);
        });
        return completed;
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
            stopped = !visit_overlay_rows(state, hash, kind, visitor);
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

Fid IndexQuery::definition_file(SymbolHash hash) const {
    Fid found;
    for_each_relation(hash,
                      {},
                      RelationKind::Definition,
                      Order::LiveFirst,
                      {.shard = false, .preamble = false, .overlay = false},
                      [&](const RowSource& source, const Relation&) {
                          found = source.file;
                          return false;
                      });
    if(found.valid()) {
        return found;
    }
    auto identity = index.identity_of(hash);
    if(!identity || !has_flag(identity->flags, SymbolFlags::HasDefinition)) {
        return {};
    }
    // The table never retracts a definition: a file reindexed without it
    // keeps the record until another unit defines the symbol.
    Fid file{identity->file};
    auto* rows = index.shard(file);
    bool defines = false;
    if(rows) {
        rows->lookup(hash, RelationKind::Definition, [&](const Relation&) {
            defines = true;
            return false;
        });
    }
    return defines ? file : Fid{};
}

std::optional<IndexQuery::Cursor> IndexQuery::symbol_at(Fid file, std::uint32_t offset) const {
    auto source = serving(file);
    if(!source) {
        return std::nullopt;
    }
    std::optional<Cursor> cursor;
    auto hit = [&](const Shard& rows) {
        auto named = named_at(rows, offset, [&](SymbolHash hash) {
            auto info = symbol_info(hash, file);
            return info && info->kind == SymbolKind::Macro;
        });
        if(!named) {
            return false;
        }
        auto site = RowSource{.file = file,
                              .path = source->path,
                              .rows = &rows,
                              .positions = source->positions}
                        .site(named->range);
        if(!site) {
            return false;
        }
        cursor = Cursor{.symbols = std::move(named->symbols), .site = *site};
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
    auto offset = source->positions.offset({.line = line, .character = utf16_column},
                                           PositionEncoding::UTF16);
    if(!offset) {
        return std::nullopt;
    }
    return symbol_at(file, *offset);
}

std::optional<SymbolRef> IndexQuery::symbol_info(SymbolHash hash, Fid anchor) const {
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

    if(auto identity = index.identity_of(hash)) {
        return SymbolRef::from(hash, *identity);
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
    if(auto* shard = anchor.valid() ? index.shard(anchor) : nullptr) {
        if(auto identity = shard->find_symbol(hash)) {
            return SymbolRef::from(hash, *identity);
        }
    }
    for(auto& [path_id, shard]: index.shards) {
        if(!shard.loaded()) {
            continue;
        }
        if(auto identity = shard.find_symbol(hash)) {
            adopt(*identity);
            return found;
        }
    }
    return std::nullopt;
}

llvm::SmallVector<SymbolRef, 4> IndexQuery::container_chain(SymbolHash hash) const {
    auto symbol = symbol_info(hash);
    if(!symbol) {
        return {};
    }
    return scope_chain(hash, symbol->parent, [&](SymbolHash scope) { return symbol_info(scope); });
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

std::vector<Site> IndexQuery::sites(SymbolHash hash, Fid anchor, RelationKind kind) const {
    std::vector<Site> result;
    for_each_relation(hash,
                      anchor,
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

std::optional<IndexQuery::Placed> IndexQuery::first_placed(SymbolHash hash,
                                                           Fid anchor,
                                                           RelationKind kind) const {
    std::optional<Placed> result;
    for_each_relation(hash,
                      anchor,
                      kind,
                      Order::LiveFirst,
                      {},
                      [&](const RowSource& source, const Relation& relation) {
                          if(auto site = source.site(relation.range)) {
                              result = Placed{.site = *site, .extent = *site};
                              if(auto extent = extent_of(source, relation)) {
                                  result->extent = *extent;
                              }
                          }
                          return !result;
                      });
    return result;
}

std::optional<Site> IndexQuery::first_site(SymbolHash hash, Fid anchor, RelationKind kind) const {
    auto placed = first_placed(hash, anchor, kind);
    return placed ? std::optional(placed->site) : std::nullopt;
}

std::optional<IndexQuery::Placed> IndexQuery::canonical_placed(SymbolHash hash, Fid anchor) const {
    if(auto placed = first_placed(hash, anchor, RelationKind::Definition)) {
        return placed;
    }
    // A declaration stands in only for a symbol nothing defines. A
    // reported definition may sit in a file whose rows are not serving —
    // withheld as stale, or an open buffer that moved on from them — and
    // then stays unavailable (`clice query` reports such a symbol as not
    // found). A deleted file holds nothing anymore, and a definition
    // deleted since its report leaves no such row behind.
    if(reported_defined(hash, anchor)) {
        bool unavailable = false;
        each_disk_file(hash, anchor, [&](Fid file) {
            auto* shard = index.shard(file);
            if(unavailable || !shard || serving(file) || files.seen_missing(file)) {
                return;
            }
            shard->lookup(hash, RelationKind::Definition, [&](const Relation&) {
                unavailable = true;
                return false;
            });
        });
        if(unavailable) {
            return std::nullopt;
        }
    }
    return first_placed(hash, anchor, RelationKind::Declaration);
}

bool IndexQuery::reported_defined(SymbolHash hash, Fid anchor) const {
    if(auto info = symbol_info(hash, anchor);
       info && has_flag(info->flags, SymbolFlags::HasDefinition)) {
        return true;
    }
    auto row = index.identity_of(hash);
    return row && has_flag(row->flags, SymbolFlags::HasDefinition);
}

std::vector<IndexQuery::Edge> IndexQuery::edges(SymbolHash hash,
                                                Fid anchor,
                                                RelationKind kind) const {
    // The main-file preamble entry cannot contribute: the preamble region
    // holds only preprocessor directives, never call or type relations.
    llvm::DenseMap<SymbolHash, std::vector<Site>> by_target;
    for_each_relation(hash,
                      anchor,
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
        auto located = resolve(target, sites.front().file);
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
                                                             Fid anchor,
                                                             RelationKind kind) const {
    std::vector<Located> result;
    for(auto target: targets(hash, anchor, kind)) {
        if(auto located = resolve(target.symbol, target.anchor)) {
            result.push_back(std::move(*located));
        }
    }
    return result;
}

IndexQuery::CallGraph IndexQuery::call_graph(SymbolHash root,
                                             Fid anchor,
                                             CallGraphOptions options) const {
    CallGraph graph;
    if(options.callers) {
        graph.callers = edges(root, anchor, RelationKind::Caller);
    }
    if(options.callees) {
        graph.callees = edges(root, anchor, RelationKind::Callee);
    }
    return graph;
}

IndexQuery::TypeHierarchy IndexQuery::type_hierarchy(SymbolHash root,
                                                     Fid anchor,
                                                     TypeHierarchyOptions options) const {
    TypeHierarchy hierarchy;
    if(options.supertypes) {
        hierarchy.supertypes = located_targets(root, anchor, RelationKind::Base);
    }
    if(options.subtypes) {
        hierarchy.subtypes = located_targets(root, anchor, RelationKind::Derived);
    }
    return hierarchy;
}

llvm::SmallVector<IndexQuery::Target> IndexQuery::targets(SymbolHash hash,
                                                          Fid anchor,
                                                          RelationKind kind) const {
    llvm::SmallVector<Target> result;
    llvm::DenseSet<SymbolHash> seen;
    for_each_relation(hash,
                      anchor,
                      kind,
                      Order::DiskFirst,
                      {.preamble = false},
                      [&](const RowSource& source, const Relation& relation) {
                          if(seen.insert(relation.target_symbol).second) {
                              result.push_back(
                                  {.symbol = relation.target_symbol, .anchor = source.file});
                          }
                          return true;
                      });
    return result;
}

std::vector<Site> IndexQuery::definition(const Cursor& cursor) const {
    ScopedTimer timer;
    auto anchor = cursor.site.file;
    std::vector<Site> result;
    for(auto symbol: cursor.symbols) {
        auto found = sites(symbol, anchor, RelationKind::Definition);
        if(found.empty() || std::ranges::any_of(found, [&](const Site& site) {
               return covers(site, cursor.site);
           })) {
            llvm::append_range(found, sites(symbol, anchor, RelationKind::Declaration));
            dedup_sites(found);
            drop_cursor_site(found, cursor.site);
        }
        llvm::append_range(result, std::move(found));
    }
    dedup_sites(result);
    LOG_PERF("index_query",
             "kind=definition path={} results={} elapsed_ms={:.2f}",
             cursor.site.path,
             result.size(),
             timer.ms_f());
    return result;
}

std::vector<Site> IndexQuery::declaration(const Cursor& cursor) const {
    ScopedTimer timer;
    std::vector<Site> result;
    for(auto symbol: cursor.symbols) {
        for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
            llvm::append_range(result, sites(symbol, cursor.site.file, kind));
        }
    }
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
    llvm::SmallVector<RelationKind, 4> kinds{RelationKind::Reference, RelationKind::WeakReference};
    if(include_declaration) {
        kinds.append({RelationKind::Declaration, RelationKind::Definition});
    }
    std::vector<Site> result;
    for(auto symbol: cursor.symbols) {
        for(auto kind: kinds) {
            llvm::append_range(result, sites(symbol, cursor.site.file, kind));
        }
    }
    dedup_sites(result);
    LOG_PERF("index_query",
             "kind=references path={} results={} elapsed_ms={:.2f}",
             cursor.site.path,
             result.size(),
             timer.ms_f());
    return result;
}

std::vector<IndexQuery::PlacedHighlight>
    IndexQuery::document_highlights(const Cursor& cursor) const {
    auto source = serving(cursor.site.file);
    if(!source) {
        return {};
    }
    std::vector<PlacedHighlight> result;
    auto place = [&](llvm::ArrayRef<const Shard*> rows) {
        for(auto& highlight: highlights(rows, cursor.symbols)) {
            if(auto site = source->site(highlight.range)) {
                result.push_back({.site = std::move(*site), .kind = highlight.kind});
            }
        }
    };
    // The preamble rows live only as long as the visit.
    bool placed = false;
    if(source->kind == RowSource::Kind::SessionRows) {
        live->each_preamble([&](const RowSource& preamble) {
            if(preamble.file != cursor.site.file) {
                return true;
            }
            place({source->rows, preamble.rows});
            placed = true;
            return false;
        });
    }
    if(!placed) {
        place({source->rows});
    }
    return result;
}

std::vector<Site> IndexQuery::target_sites(SymbolHash hash, Fid anchor, RelationKind kind) const {
    std::vector<Site> result;
    for(auto& located: located_targets(hash, anchor, kind)) {
        result.push_back(located.site);
    }
    return result;
}

bool IndexQuery::deduced(const Cursor& cursor, SymbolHash symbol) const {
    auto source = serving(cursor.site.file);
    if(!source) {
        return false;
    }
    bool found = false;
    auto hit = [&](const Shard& rows) {
        rows.lookup(symbol, RelationKind::Deduced, [&](const Relation& relation) {
            found = relation.range == cursor.site.range;
            return !found;
        });
        return found;
    };
    if(hit(*source->rows) || source->kind != RowSource::Kind::SessionRows) {
        return found;
    }
    live->each_preamble([&](const RowSource& preamble) {
        if(preamble.file != cursor.site.file) {
            return true;
        }
        hit(*preamble.rows);
        return false;
    });
    return found;
}

std::vector<Site> IndexQuery::implementation(SymbolHash hash, Fid anchor) const {
    auto info = symbol_info(hash, anchor);
    if(!info) {
        return {};
    }
    if(info->kind == SymbolKind::Class || info->kind == SymbolKind::Struct ||
       info->kind == SymbolKind::Union) {
        return target_sites(hash, anchor, RelationKind::Derived);
    }
    // An override that only declares — a pure virtual of an abstract
    // intermediate class — is listed, and its own overriders after it.
    std::vector<Site> result;
    llvm::DenseSet<SymbolHash> seen{hash};
    llvm::SmallVector<Target> pending{
        {.symbol = hash, .anchor = anchor}
    };
    while(!pending.empty()) {
        auto next = pending.pop_back_val();
        for(auto& located:
            located_targets(next.symbol, next.anchor, RelationKind::Implementation)) {
            if(!seen.insert(located.symbol.hash).second) {
                continue;
            }
            result.push_back(located.site);
            if(!reported_defined(located.symbol.hash, located.site.file)) {
                pending.push_back({.symbol = located.symbol.hash, .anchor = located.site.file});
            }
        }
    }
    return result;
}

std::optional<llvm::StringRef>
    IndexQuery::source_text(const RowSource& source,
                            std::unique_ptr<llvm::MemoryBuffer>& storage) const {
    // Buffer-backed sources and non-ASCII blobs carry their text; an
    // open session served by its shard reads in buffer coordinates, so it
    // lands here too.
    if(source.kind == RowSource::Kind::SessionRows ||
       source.kind == RowSource::Kind::PreambleRows || !source.positions.text().empty()) {
        return source.positions.text();
    }
    return disk_text(source.path, *source.rows, storage);
}

std::optional<IndexQuery::Definition> IndexQuery::definition_text(SymbolHash hash,
                                                                  Fid anchor) const {
    // Live sources first: buffer-true rows also know symbols the project
    // table has never seen (an unsaved definition), the preamble region
    // holds the buffer's own macros, and an overlay is the only source for
    // a header seen under the live context. The first in-bounds Definition
    // payload whose text is available wins.
    std::optional<Definition> found;
    for_each_relation(hash,
                      anchor,
                      RelationKind::Definition,
                      Order::LiveFirst,
                      {},
                      [&](const RowSource& source, const Relation& relation) {
                          auto extent = extent_of(source, relation);
                          if(!extent) {
                              return true;
                          }
                          std::unique_ptr<llvm::MemoryBuffer> storage;
                          auto text = source_text(source, storage);
                          if(!text) {
                              return true;
                          }
                          auto range = extent->range;
                          found = Definition{
                              .extent = *extent,
                              .text = std::string(text->substr(range.begin, range.length())),
                              .comment = preceding_comment(*text, range.begin),
                          };
                          return false;
                      });
    return found;
}

std::optional<std::string> IndexQuery::serving_text(Fid file) const {
    auto source = serving(file);
    if(!source) {
        return std::nullopt;
    }
    std::unique_ptr<llvm::MemoryBuffer> storage;
    auto text = source_text(*source, storage);
    return text ? std::optional(text->str()) : std::nullopt;
}

std::string IndexQuery::context_line(const Site& site) const {
    if(!site.file.valid()) {
        return {};
    }
    if(auto source = serving(site.file); source && !source->positions.text().empty()) {
        return extract_line(source->positions.text(), site.range.begin);
    }
    auto* shard = index.shard(site.file);
    if(!shard) {
        return {};
    }
    std::unique_ptr<llvm::MemoryBuffer> storage;
    auto text = disk_text(site.path, *shard, storage);
    return text ? extract_line(*text, site.range.begin) : std::string{};
}

std::vector<IndexQuery::Located> IndexQuery::resolve_at(const Cursor& cursor) const {
    std::vector<Located> result;
    for(auto symbol: cursor.symbols) {
        if(auto located = resolve(symbol, cursor.site.file)) {
            result.push_back(std::move(*located));
        }
    }
    return result;
}

std::optional<IndexQuery::Located> IndexQuery::resolve(SymbolHash hash, Fid anchor) const {
    auto info = symbol_info(hash, anchor);
    if(!info) {
        return std::nullopt;
    }
    auto placed = canonical_placed(hash, anchor);
    if(!placed) {
        return std::nullopt;
    }
    return Located{.symbol = std::move(*info), .site = placed->site, .extent = placed->extent};
}

IndexQuery::RankedHits IndexQuery::ranked_search(const SymbolQuery& query,
                                                 std::size_t limit) const {
    std::vector<Ranked> hits;
    llvm::DenseSet<SymbolHash> seen;
    auto indexed = index.search_index.search(query, limit, index.workspace);
    // A damaged index answers incompletely, a missing one not at all:
    // until the rebuild the whole table is judged row by row instead.
    bool scan_table = index.search_index.damaged() || !index.search_index.loaded();
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
    bool scoped =
        query.absolute || !query.scope.empty() || query.mode == SymbolQuery::Mode::Members;
    // Candidates share their containers: each is looked up once per
    // search, not once per candidate through every open session's table.
    llvm::DenseMap<SymbolHash, std::optional<SymbolRef>> scopes;
    auto scope_info = [&](SymbolHash scope) {
        auto [it, inserted] = scopes.try_emplace(scope);
        if(inserted) {
            it->second = symbol_info(scope);
        }
        return it->second;
    };
    auto consider = [&](SymbolHash hash,
                        const SymbolIdentity& identity,
                        llvm::StringRef path,
                        std::uint32_t reference_files) {
        if(!is_searchable_kind(identity.kind) || identity.name.empty() ||
           has_flag(identity.flags, SymbolFlags::Unnamed) || seen.contains(hash)) {
            return;
        }
        if(!query.kinds.empty() && !llvm::is_contained(query.kinds, identity.kind)) {
            return;
        }
        if(!query.paths.empty() && llvm::none_of(query.paths, [&](const std::string& wanted) {
               return !path.empty() && path_matches(wanted, path);
           })) {
            return;
        }
        auto quality =
            symbol_quality(identity.name, identity.kind, identity.flags, reference_files);
        auto rank = ranker.rank(identity.name, identity.args, quality, /*lenient=*/true);
        if(!rank) {
            return;
        }
        if(scoped) {
            auto containers = scope_chain(hash, identity.parent, scope_info);
            llvm::SmallVector<ScopeEntry, 4> chain;
            for(auto& container: containers) {
                chain.push_back({.name = container.name, .args = container.args});
            }
            if(!in_scope(query, chain)) {
                return;
            }
        }
        seen.insert(hash);
        hits.push_back({*rank, SymbolRef::from(hash, identity)});
    };
    auto references_of = [&](SymbolHash hash) -> std::uint32_t {
        return index.reference_count(hash);
    };
    auto consider_row =
        [&](SymbolHash hash, const SymbolIdentity& symbol, std::uint32_t references) {
            llvm::StringRef path;
            if(symbol.file != no_file) {
                path = files.resolve(Fid{symbol.file});
            }
            consider(hash, symbol, path, references);
        };
    if(scan_table) {
        index.for_each_symbol(
            [&](SymbolHash hash, const SymbolIdentity& symbol, std::uint32_t references) {
                consider_row(hash, symbol, references);
                return true;
            });
    } else {
        for(auto hash: index.search_pending) {
            if(auto symbol = index.identity_of(hash)) {
                consider_row(hash, *symbol, index.reference_count(hash));
            }
        }
    }
    if(live) {
        live->each_session_index([&](const TUIndex& state) {
            state.iterate_symbols([&](SymbolHash hash,
                                      const SymbolIdentity& identity,
                                      llvm::ArrayRef<std::uint32_t>) {
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
            if(auto placed = canonical_placed(hit.symbol.hash, Fid{})) {
                results.push_back({.symbol = std::move(hit.symbol),
                                   .site = placed->site,
                                   .extent = placed->extent});
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
        Fid anchor;
        for(auto& path: query.paths) {
            if(auto file = files.find(Spelling::absolute(path))) {
                anchor = *file;
                break;
            }
        }
        if(auto located = resolve(*query.handle, anchor)) {
            return {std::move(*located)};
        }
        return {};
    }

    if(query.position) {
        auto& place = *query.position;
        auto path_id = files.find(Spelling::absolute(place.path));
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
        auto bounds = source->positions.line_bounds(line);
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
            return resolve_at(*cursor);
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
            if(auto info = symbol_info(hash, *path_id)) {
                defined.push_back({.symbol = std::move(*info),
                                   .site = *site,
                                   .extent = extent_of(*source, r).value_or(*site)});
            }
            return true;
        });
        return defined;
    }

    auto ranked = ranked_search(query, 50).hits;
    // Spelling the name exactly settles it; otherwise every match stands.
    // A class and its constructors spell one name: asked by it, the class
    // answers.
    llvm::DenseSet<SymbolHash> exact;
    for(auto& hit: ranked) {
        if(hit.rank.tier <= 1) {
            exact.insert(hit.symbol.hash);
        }
    }
    std::vector<Located> results;
    for(auto& hit: ranked) {
        if(!exact.empty() && hit.rank.tier > 1) {
            continue;
        }
        if(name_form(hit.symbol.flags) == NameForm::Constructor &&
           exact.contains(hit.symbol.parent)) {
            continue;
        }
        if(auto placed = canonical_placed(hit.symbol.hash, Fid{})) {
            results.push_back(
                {.symbol = std::move(hit.symbol), .site = placed->site, .extent = placed->extent});
        }
    }
    return results;
}

std::vector<IndexQuery::Located> IndexQuery::definitions_in(Fid file) const {
    auto source = serving(file);
    if(!source) {
        return {};
    }
    // The rows themselves say what is defined in the file, in row order:
    // the project table's symbols and the file's internal ones — a
    // function's locals are no part of it.
    std::vector<Located> result;
    source->rows->for_each_relation([&](SymbolHash hash, const Relation& r) {
        if(r.kind != RelationKind::Definition) {
            return true;
        }
        auto identity = index.identity_of(hash);
        if(!identity) {
            identity = source->rows->find_symbol(hash);
            if(identity && identity->scope != SymbolScope::TULocal) {
                return true;
            }
        }
        if(!identity || identity->name.empty()) {
            return true;
        }
        if(auto site = source->site(r.range)) {
            result.push_back({.symbol = SymbolRef::from(hash, *identity),
                              .site = *site,
                              .extent = extent_of(*source, r).value_or(*site)});
        }
        return true;
    });
    return result;
}

std::vector<IncludeEdge> IndexQuery::include_edges(Fid file) const {
    // Every consumer projects the edges onto content the serving shard
    // matches, so a manifest contributes only where the version it entered
    // for this document carries that same content generation — a TU that
    // indexed an older revision would place its lines in text that moved.
    auto* shard = index.shard(file);
    if(!shard) {
        return {};
    }
    auto generation = shard->content_hash();

    auto is_document = [&](VersionID fv) {
        auto& version = files.version(fv);
        return version.fid == file && version.content_hash == generation;
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
            edges.push_back({
                .line = node.line,
                .target = std::string(files.resolve(files.version(VersionID{node.file}).fid)),
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

auto preceding_comment(llvm::StringRef content, std::uint32_t offset) -> std::string {
    if(offset > content.size()) {
        return {};
    }

    // Walk to the start of the line containing `offset`, then collect the
    // contiguous run of comment-looking lines directly above it.
    auto begin = line_begin(content, offset);
    llvm::SmallVector<llvm::StringRef, 8> lines;
    // Between a closing */ and its opener the scan is inside a block
    // comment: interior lines need no marker of their own (`/*` above bare
    // text above `*/`). `block_start` remembers where the block began in
    // `lines` so one whose opener never surfaces can be dropped.
    bool in_block = false;
    std::size_t block_start = 0;
    while(begin > 0) {
        auto prev_begin = line_begin(content, begin - 1);
        auto line = content.substr(prev_begin, begin - prev_begin).rtrim("\r\n").trim();
        if(in_block) {
            // An opener sharing its line with code marks a comment trailing
            // that code, not documentation of the decl below.
            if(line.contains("/*") && !line.starts_with("/*")) {
                break;
            }
            lines.push_back(line);
            if(line.starts_with("/*")) {
                in_block = false;
            }
        } else {
            // A trailing */ only marks a comment line when the opener is on
            // an earlier line: `int a; /* note */` closes a comment it
            // opened itself, and everything before the marker is code.
            bool closes_block = line.ends_with("*/") && !line.contains("/*");
            bool comment_like = line.starts_with("//") || line.starts_with("/*") ||
                                line.starts_with("*") || closes_block;
            if(!comment_like || line.empty()) {
                break;
            }
            if(closes_block) {
                in_block = true;
                block_start = lines.size();
            }
            lines.push_back(line);
        }
        begin = prev_begin;
    }
    if(in_block) {
        lines.truncate(block_start);
    }

    std::string result;
    for(auto& line: llvm::reverse(lines)) {
        auto text = line;
        for(llvm::StringRef marker: {"///<", "///", "//!", "//", "/**", "/*", "*/"}) {
            if(text.starts_with(marker)) {
                text = text.drop_front(marker.size());
                break;
            }
        }
        if(text.starts_with("*")) {
            text = text.drop_front(1);
        }
        text = text.rtrim("*/").trim();
        if(!result.empty()) {
            result += '\n';
        }
        result += text.str();
    }
    return llvm::StringRef(result).trim().str();
}

}  // namespace clice::index
