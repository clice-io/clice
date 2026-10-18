module;

#include "modules/prelude.h"

module clice;

import :index.query;
import :vfs.file_system;

namespace clice::index {

std::optional<llvm::StringRef> disk_text(llvm::StringRef path,
                                         const Shard& shard,
                                         std::unique_ptr<llvm::MemoryBuffer>& kept) {
    auto buffer = vfs::read(path);
    if(!buffer) {
        return std::nullopt;
    }
    auto text = (*buffer)->getBuffer();
    if(llvm::xxh3_64bits(text) != shard.content_hash()) {
        return std::nullopt;
    }
    kept = std::move(*buffer);
    return text;
}

std::optional<Site> extent_of(const IndexSource& source, const Relation& relation) {
    assert(RelationKind(relation.kind).isDeclOrDef());
    auto extent = std::bit_cast<LocalSourceRange>(relation.target);
    if(extent.begin >= extent.end) {
        return std::nullopt;
    }
    return source.site(extent);
}

llvm::SmallVector<SymbolInfo, 4>
    scope_chain(SymbolHash hash,
                SymbolHash parent,
                llvm::function_ref<std::optional<SymbolInfo>(SymbolHash)> lookup) {
    llvm::SmallVector<SymbolInfo, 4> chain;
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

bool declares(const Shard& shard, SymbolHash hash) {
    bool found = false;
    for(auto kind: {RelationKind::Definition, RelationKind::Declaration}) {
        shard.relations_of(hash, kind, [&](const Relation&) {
            found = true;
            return false;
        });
        if(found) {
            return true;
        }
    }
    return false;
}

bool covers(const Site& site, const Site& cursor) {
    return site.path == cursor.path && site.range.contains(cursor.range);
}

void dedup_sites(std::vector<Site>& sites) {
    std::ranges::sort(sites, [](const Site& lhs, const Site& rhs) {
        return std::tuple(site_key(lhs), lhs.origin) < std::tuple(site_key(rhs), rhs.origin);
    });
    auto dup = std::ranges::unique(sites, [](const Site& lhs, const Site& rhs) {
        return site_key(lhs) == site_key(rhs);
    });
    sites.erase(dup.begin(), dup.end());
}

bool resolved_relations(const InstantiationShard& shard,
                        SymbolHash symbol,
                        RelationKind kind,
                        llvm::function_ref<bool(const Relation&, bool expansion)> visit) {
    bool completed = true;
    shard.records_of(symbol, [&](const InstantiationRecord& record) {
        std::optional<Relation> relation;
        bool names = record.target == symbol;
        switch(record.kind) {
            case InstantiationKind::Reference: {
                if(names && kind.is_one_of(RelationKind::Reference)) {
                    relation = Relation{.kind = RelationKind::Reference, .range = record.range};
                }
                break;
            }
            case InstantiationKind::Deduced: {
                if(names && kind.is_one_of(RelationKind::Deduced)) {
                    relation = Relation{.kind = RelationKind::Deduced, .range = record.range};
                }
                break;
            }
            case InstantiationKind::Implied: {
                if(names && kind.is_one_of(RelationKind::Implied)) {
                    relation = Relation{.kind = RelationKind::Implied, .range = record.range};
                }
                break;
            }
            case InstantiationKind::Call: {
                if(names && kind.is_one_of(RelationKind::Caller)) {
                    relation = Relation{.kind = RelationKind::Caller,
                                        .range = record.range,
                                        .target = record.from};
                } else if(record.from == symbol && kind.is_one_of(RelationKind::Callee)) {
                    relation = Relation{.kind = RelationKind::Callee,
                                        .range = record.range,
                                        .target = record.target};
                }
                break;
            }
            case InstantiationKind::Base: {
                // Base and derived relations pair symbols without a range.
                if(names && kind.is_one_of(RelationKind::Reference)) {
                    relation = Relation{.kind = RelationKind::Reference, .range = record.range};
                } else if(names && kind.is_one_of(RelationKind::Derived)) {
                    relation =
                        Relation{.kind = RelationKind::Derived, .target = record.from};
                } else if(record.from == symbol && kind.is_one_of(RelationKind::Base)) {
                    relation = Relation{.kind = RelationKind::Base, .target = record.target};
                }
                break;
            }
            case InstantiationKind::TypeDefinition: {
                // The type a declaration's placeholder takes, as the
                // declared type's relation carries no range.
                if(record.from == symbol && kind.is_one_of(RelationKind::TypeDefinition)) {
                    relation = Relation{.kind = RelationKind::TypeDefinition,
                                        .target = record.target};
                }
                break;
            }
        }
        completed = !relation || visit(*relation, record.expansion);
        return completed;
    });
    return completed;
}

std::optional<Named> named_at(const Shard& shard,
                              std::uint32_t offset,
                              llvm::function_ref<bool(SymbolHash)> is_macro) {
    llvm::SmallVector<Occurrence, 2> candidates;
    shard.occurrences_at(offset, [&](const Occurrence& occurrence) {
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
    // Only macros here: the definitions a shared header's variants expand.
    if(named.symbols.empty()) {
        for(auto& candidate: candidates) {
            named.symbols.push_back(candidate.target);
        }
    }
    return named;
}

void add_resolved(const InstantiationShard& shard,
                  std::uint32_t offset,
                  std::optional<Named>& named) {
    shard.records_at(offset, [&](const InstantiationRecord& record) {
        if(record.kind == InstantiationKind::Call ||
           record.kind == InstantiationKind::TypeDefinition ||
           (named && !(named->range == record.range))) {
            return true;
        }
        if(!named) {
            named = Named{.range = record.range};
        }
        if(!llvm::is_contained(named->symbols, record.target)) {
            named->symbols.push_back(record.target);
        }
        return true;
    });
}

bool FreshnessGate::stale(Fid file, std::uint64_t content_hash) const {
    if(!options.withhold) {
        return false;
    }
    if(options.check_disk && checked.insert(file).second) {
        files.disk.current(file);
    }
    if(!files.disk.seen_missing(file)) {
        auto seen = files.disk.seen_hash(file);
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

std::optional<IndexSource> IndexQuery::serving(Fid file) const {
    if(live && live->is_open(file)) {
        return live->buffer_index(file);
    }
    auto* shard = index.shard(file);
    if(!shard || (gate && gate->stale(file, shard->content_hash()))) {
        return std::nullopt;
    }
    IndexSource source{.kind = IndexSource::Kind::Shard,
                       .file = file,
                       .path = files.display(file),
                       .shard = shard,
                       .positions = shard->positions()};
    if(auto* instantiations = instantiations_of(file, *shard)) {
        source.instantiations.push_back(instantiations);
    }
    return source;
}

const InstantiationShard* IndexQuery::instantiations_of(Fid file, const Shard& shard) const {
    if(!with_instantiations) {
        return nullptr;
    }
    auto* instantiations = index.instantiation_shard(file);
    return instantiations && instantiations->content_hash() == shard.content_hash() ? instantiations
                                                                                    : nullptr;
}

const Shard* IndexQuery::shard_matching(Fid file, llvm::StringRef text) const {
    auto* shard = index.shard(file);
    return shard && shard->matches_content(text) ? shard : nullptr;
}

std::shared_ptr<TUIndex> IndexQuery::preamble_index(Fid file) const {
    return live ? live->preamble_index(file) : nullptr;
}

bool IndexQuery::visit_overlay_relations(const TUIndex& state,
                                         SymbolHash hash,
                                         RelationKind kind,
                                         RelationVisitor visitor) const {
    auto focus_id = state.path_count() - 1;
    bool stopped = false;
    // What the TU's instantiations resolve in its other files, in the
    // coordinates of the text the TU read: the overlay's own entries of
    // the file, or the disk shard of that very text.
    for(std::uint32_t i = 0; with_instantiations && i < state.instantiation_count() && !stopped; i += 1) {
        auto local_id = state.instantiation_path(i);
        auto& instantiations = state.instantiation_shard(i);
        if(local_id == focus_id || !instantiations.loaded()) {
            continue;
        }
        auto file = files.intern(Spelling::absolute(state.path(local_id)));
        const Shard* shard = &state.shard_of(local_id);
        if(!shard->loaded()) {
            shard = index.shard(file);
        }
        if(!shard || shard->content_hash() != instantiations.content_hash() ||
           live->is_open(file) || (gate && gate->stale(file, shard->content_hash()))) {
            continue;
        }
        IndexSource instantiated{.kind = IndexSource::Kind::Overlay,
                                 .file = file,
                                 .path = files.display(file),
                                 .shard = shard,
                                 .positions = shard->positions(),
                                 .origin = SiteOrigin::Instantiation};
        auto expanded = instantiated;
        expanded.origin = SiteOrigin::Expansion;
        stopped =
            !resolved_relations(instantiations,
                                hash,
                                kind,
                                [&](const Relation& relation, bool expansion) {
                                    return visitor(expansion ? expanded : instantiated, relation);
                                });
    }
    for(std::uint32_t i = 0; i < state.section_count() && !stopped; i += 1) {
        auto local_id = state.section_path(i);
        if(local_id == focus_id) {
            continue;
        }
        auto& shard = state.section_shard(i);
        std::optional<IndexSource> source;
        shard.relations_of(hash, kind, [&](const Relation& relation) {
            // An overlay spans hundreds of headers and few hold relations of
            // any one symbol: resolving every entry's file up front dominated
            // each lookup.
            if(!source) {
                auto file = files.intern(Spelling::absolute(state.path(local_id)));
                if(live->is_open(file) || (gate && gate->stale(file, shard.content_hash()))) {
                    return false;
                }
                source = IndexSource{.kind = IndexSource::Kind::Overlay,
                                     .file = file,
                                     .path = files.display(file),
                                     .shard = &shard,
                                     .positions = shard.positions()};
            }
            stopped = !visitor(*source, relation);
            return !stopped;
        });
    }
    return !stopped;
}

void IndexQuery::for_each_disk_file(SymbolHash hash,
                                Fid anchor,
                                llvm::function_ref<void(Fid)> visit) const {
    if(index.find_symbol(hash)) {
        index.for_each_reference_file(hash, visit);
    } else if(anchor.valid()) {
        index.for_each_file_of_local(hash, anchor, visit);
    }
}

void IndexQuery::for_each_relation(SymbolHash hash,
                                   Fid anchor,
                                   RelationKind kind,
                                   Order order,
                                   SourceMask mask,
                                   RelationVisitor visitor) const {
    bool stopped = false;
    auto emit = [&](const IndexSource& source) {
        source.shard->relations_of(hash, kind, [&](const Relation& relation) {
            if(!visitor(source, relation)) {
                stopped = true;
                return false;
            }
            return true;
        });
        if(stopped || !with_instantiations || source.instantiations.empty()) {
            return !stopped;
        }
        auto instantiated = source;
        instantiated.origin = SiteOrigin::Instantiation;
        auto expanded = source;
        expanded.origin = SiteOrigin::Expansion;
        for(auto* instantiations: source.instantiations) {
            stopped = !resolved_relations(*instantiations,
                                          hash,
                                          kind,
                                          [&](const Relation& relation, bool expansion) {
                                              return visitor(expansion ? expanded : instantiated,
                                                             relation);
                                          });
            if(stopped) {
                break;
            }
        }
        return !stopped;
    };

    auto disk = [&] {
        if(!mask.shard) {
            return true;
        }
        bool completed = true;
        llvm::SmallDenseSet<Fid, 8> seen;
        auto visit = [&](Fid file) {
            if(!completed || !seen.insert(file).second) {
                return;
            }
            auto source = serving(file);
            if(!source || source->kind != IndexSource::Kind::Shard) {
                return;
            }
            completed = emit(*source);
        };
        for_each_disk_file(hash, anchor, visit);
        if(with_instantiations) {
            index.for_each_instantiation_file(hash, visit);
        }
        return completed;
    };
    auto sessions = [&] {
        if(!mask.session || !live) {
            return true;
        }
        live->for_each_session(emit);
        return !stopped;
    };
    auto preambles = [&] {
        if(!mask.preamble || !live) {
            return true;
        }
        live->for_each_preamble(emit);
        return !stopped;
    };
    auto overlays = [&] {
        if(!mask.overlay || !live) {
            return true;
        }
        auto visit = [&](const TUIndex& state) {
            stopped = !visit_overlay_relations(state, hash, kind, visitor);
            return !stopped;
        };
        live->for_each_overlay(visit);
        if(!stopped) {
            live->for_each_session_index(visit);
        }
        return !stopped;
    };

    if(order == Order::LiveFirst) {
        // Live sources outrank disk shards: they carry the index as seen
        // under the buffer's context, and exist even when no disk TU has
        // been indexed — the in-memory-file case behind empty
        // go-to-definition. First the buffers' own indexes, then their
        // preamble regions, then the header entries.
        [[maybe_unused]] bool completed = sessions() && preambles() && overlays() && disk();
    } else {
        [[maybe_unused]] bool completed = disk() && sessions() && overlays() && preambles();
    }
}

const Shard* IndexQuery::declaring_shard(Fid file) const {
    if(auto source = serving(file)) {
        return source->shard;
    }
    return live && live->is_open(file) ? index.shard(file) : nullptr;
}

void IndexQuery::for_each_live_file(SymbolHash hash,
                                RelationKind kind,
                                llvm::function_ref<void(Fid)> visit) const {
    // One source's relations arrive together.
    Fid last;
    for_each_relation(hash,
                      {},
                      kind,
                      Order::LiveFirst,
                      {.shard = false, .preamble = false, .overlay = false},
                      [&](const IndexSource& source, const Relation&) {
                          if(source.file != last) {
                              last = source.file;
                              visit(source.file);
                          }
                          return true;
                      });
}

Fid IndexQuery::definition_file(SymbolHash hash) const {
    Fid found;
    for_each_live_file(hash, RelationKind::Definition, [&](Fid file) {
        if(!found.valid()) {
            found = file;
        }
    });
    if(found.valid()) {
        return found;
    }
    auto identity = index.find_symbol(hash);
    if(!identity || !has_flag(identity->flags, SymbolFlags::HasDefinition)) {
        return {};
    }
    // The table never retracts a definition: a file reindexed or edited
    // without it keeps the record until another unit defines the symbol.
    Fid file{identity->file};
    auto* shard = declaring_shard(file);
    bool defines = false;
    if(shard) {
        shard->relations_of(hash, RelationKind::Definition, [&](const Relation&) {
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
    auto is_macro = [&](SymbolHash hash) {
        auto info = symbol_info(hash, file);
        return info && info->kind == SymbolKind::Macro;
    };
    // A separator — `:`, `=`, `&&`, `||`, `?` — leading to what it does not
    // spell (a range-for's `begin`, a copy's constructor) gives way to the
    // name it follows: `for(auto& x: xs)` with the cursor right after `x`
    // stands on `x`.
    std::unique_ptr<llvm::MemoryBuffer> kept;
    auto separates = [&](const Named& named) {
        if(named.range.begin != offset || named.range.length() > 2 ||
           llvm::any_of(named.symbols, [&](SymbolHash hash) {
               auto info = symbol_info(hash, file);
               return info && name_form(info->flags) == NameForm::Operator;
           })) {
            return false;
        }
        auto text = source_text(*source, kept);
        return text && llvm::is_contained({":", "=", "&&", "||", "?"},
                                          text->slice(named.range.begin, named.range.end));
    };
    auto hit = [&](const Shard& shard) {
        auto named = named_at(shard, offset, is_macro);
        if(with_instantiations && &shard == source->shard) {
            for(auto* instantiations: source->instantiations) {
                add_resolved(*instantiations, offset, named);
            }
        }
        if(!named) {
            return false;
        }
        if(offset > 0 && separates(*named)) {
            if(auto before = named_at(shard, offset - 1, is_macro);
               before && before->range.end == offset) {
                named = std::move(before);
            }
        }
        auto site = IndexSource{.file = file,
                                .path = source->path,
                                .shard = &shard,
                                .positions = source->positions}
                        .site(named->range);
        if(!site) {
            return false;
        }
        cursor = Cursor{.symbols = std::move(named->symbols), .site = *site};
        return true;
    };
    if(hit(*source->shard)) {
        return cursor;
    }
    if(source->kind != IndexSource::Kind::SessionShard) {
        return std::nullopt;
    }
    // The preamble region is compiled into the PCH and invisible to the
    // per-edit index; its occurrences (macro definitions and references
    // before the bound) live in the PCH's overlay, in the same buffer
    // coordinates — served only under the main-entry gate (preamble
    // drift, shared-PCH identity).
    live->for_each_preamble([&](const IndexSource& preamble) {
        if(preamble.file != file) {
            return true;
        }
        hit(*preamble.shard);
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

std::optional<SymbolInfo> IndexQuery::symbol_info(SymbolHash hash, Fid anchor) const {
    std::optional<SymbolInfo> found;
    auto adopt = [&](const SymbolView& identity) {
        found = SymbolInfo::from(hash, identity);
    };

    // Open sessions first: they hold every symbol of their unsaved buffers.
    if(live) {
        live->for_each_session_index([&](const TUIndex& state) {
            if(auto identity = state.find_symbol(hash)) {
                adopt(*identity);
            }
            return !found;
        });
        if(found) {
            return found;
        }
    }

    if(auto identity = index.find_symbol(hash)) {
        return SymbolInfo::from(hash, *identity);
    }

    // A symbol that exists only under an open buffer's context (or in
    // headers no disk TU has been indexed with) is in no disk table.
    if(live) {
        live->for_each_overlay([&](const TUIndex& state) {
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
            return SymbolInfo::from(hash, *identity);
        }
    }
    // An instantiation or an expansion names it in a file other than its
    // own: the files the shared locals list for it hold its declaration.
    if(anchor.valid()) {
        index.for_each_file_of_local(hash, anchor, [&](Fid file) {
            auto* shard = found ? nullptr : index.shard(file);
            if(auto identity = shard ? shard->find_symbol(hash) : std::nullopt) {
                adopt(*identity);
            }
        });
        if(found) {
            return found;
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

llvm::SmallVector<SymbolInfo, 4> IndexQuery::container_chain(SymbolHash hash, Fid anchor) const {
    auto symbol = symbol_info(hash, anchor);
    if(!symbol) {
        return {};
    }
    return scope_chain(hash, symbol->parent, [&](SymbolHash scope) {
        return symbol_info(scope, anchor);
    });
}

std::string IndexQuery::container_name(SymbolHash hash, Fid anchor) const {
    std::string result;
    for(auto& scope: container_chain(hash, anchor)) {
        if(!result.empty()) {
            result += "::";
        }
        result += scope.display_name();
    }
    return result;
}

std::string IndexQuery::qualified_name(SymbolHash hash, Fid anchor) const {
    auto symbol = symbol_info(hash, anchor);
    if(!symbol) {
        return {};
    }
    auto container = container_name(hash, anchor);
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
                      [&](const IndexSource& source, const Relation& relation) {
                          if(auto site = source.site(relation.range)) {
                              result.push_back(*site);
                          }
                          return true;
                      });
    // Same-kind relations can share one anchor: a macro body using an argument
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
                      [&](const IndexSource& source, const Relation& relation) {
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
    // reported definition may sit in a file whose shard is not serving —
    // withheld as stale, or an open buffer that moved on from it — and
    // then stays unavailable (`clice query` reports such a symbol as not
    // found). A deleted file holds nothing anymore, and a definition
    // deleted since its report leaves no such relation behind.
    if(reported_defined(hash, anchor)) {
        bool unavailable = false;
        for_each_disk_file(hash, anchor, [&](Fid file) {
            auto* shard = index.shard(file);
            if(unavailable || !shard || serving(file) || files.disk.seen_missing(file)) {
                return;
            }
            shard->relations_of(hash, RelationKind::Definition, [&](const Relation&) {
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
    auto identity = index.find_symbol(hash);
    return identity && has_flag(identity->flags, SymbolFlags::HasDefinition);
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
                      [&](const IndexSource& source, const Relation& relation) {
                          if(seen.insert(relation.target).second) {
                              result.push_back(
                                  {.symbol = relation.target, .anchor = source.file});
                          }
                          return true;
                      });
    return result;
}

llvm::SmallVector<IndexQuery::Target> IndexQuery::transitive_targets(SymbolHash hash,
                                                                     Fid anchor,
                                                                     RelationKind kind) const {
    llvm::SmallVector<Target> result;
    llvm::DenseSet<SymbolHash> seen{hash};
    llvm::SmallVector<Target> pending{
        {.symbol = hash, .anchor = anchor}
    };
    while(!pending.empty()) {
        auto next = pending.pop_back_val();
        for(auto target: targets(next.symbol, next.anchor, kind)) {
            if(seen.insert(target.symbol).second) {
                result.push_back(target);
                pending.push_back(target);
            }
        }
    }
    return result;
}

std::optional<llvm::StringRef>
    IndexQuery::source_text(const IndexSource& source,
                            std::unique_ptr<llvm::MemoryBuffer>& kept) const {
    // Buffer-backed sources carry their text; an open session served by
    // its shard reads in buffer coordinates, so it ends up here too.
    if(source.kind == IndexSource::Kind::SessionShard ||
       source.kind == IndexSource::Kind::PreambleShard || !source.positions.text().empty()) {
        return source.positions.text();
    }
    return disk_text(source.path, *source.shard, kept);
}

std::optional<std::string> IndexQuery::serving_text(Fid file) const {
    auto source = serving(file);
    if(!source) {
        return std::nullopt;
    }
    std::unique_ptr<llvm::MemoryBuffer> kept;
    auto text = source_text(*source, kept);
    return text ? std::optional(text->str()) : std::nullopt;
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

}  // namespace clice::index
