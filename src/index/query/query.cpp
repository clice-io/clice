module clice;

import :index.query;
import :vfs.file_system;

namespace clice::index {

std::optional<llvm::StringRef> disk_text(llvm::StringRef path,
                                         const Shard& shard,
                                         std::string& kept) {
    auto text = vfs::read_text(path);
    if(!text || llvm::xxh3_64bits(*text) != shard.content_hash()) {
        return std::nullopt;
    }
    kept = std::move(*text);
    return llvm::StringRef(kept);
}

std::optional<Site> extent_of(const IndexSource& source, const Relation& relation) {
    assert(RelationKind(relation.kind).declares());
    auto extent = relation.extent();
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
    for_each_spelled_scope(hash, parent, lookup, [&](SymbolHash, SymbolInfo& scope) {
        chain.push_back(std::move(scope));
        return true;
    });
    ranges::reverse(chain);
    return chain;
}

bool covers(const Site& site, const Site& cursor) {
    return site.path == cursor.path && site.range.contains(cursor.range);
}

void dedup_sites(std::vector<Site>& sites) {
    ranges::sort(sites, [](const Site& lhs, const Site& rhs) {
        return std::tuple(site_key(lhs), lhs.origin) < std::tuple(site_key(rhs), rhs.origin);
    });
    auto dup = ranges::unique(sites, [](const Site& lhs, const Site& rhs) {
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
                    relation = Relation{.kind = RelationKind::Derived, .target = record.from};
                } else if(record.from == symbol && kind.is_one_of(RelationKind::Base)) {
                    relation = Relation{.kind = RelationKind::Base, .target = record.target};
                }
                break;
            }
            case InstantiationKind::TypeDefinition: {
                // The type a declaration's placeholder takes, as the
                // declared type's relation carries no range.
                if(record.from == symbol && kind.is_one_of(RelationKind::TypeDefinition)) {
                    relation =
                        Relation{.kind = RelationKind::TypeDefinition, .target = record.target};
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
                  bool expansions,
                  std::optional<Named>& named) {
    shard.records_at(offset, [&](const InstantiationRecord& record) {
        if(record.kind == InstantiationKind::Call ||
           record.kind == InstantiationKind::TypeDefinition || (record.expansion && !expansions) ||
           (named && !(named->range == record.range))) {
            return true;
        }
        if(!named) {
            named = Named{.range = record.range};
        }
        if(!ranges::contains(named->symbols, record.target)) {
            named->symbols.push_back(record.target);
        }
        return true;
    });
}

bool FreshnessGate::stale(Fid file, std::uint64_t content_hash) const {
    if(!options.withhold || !moved_on(file, content_hash)) {
        return false;
    }
    withheld_files.insert(file);
    return true;
}

bool FreshnessGate::moved_on(Fid file, std::uint64_t content_hash) const {
    if(options.check_disk && checked.insert(file).second) {
        files.disk.current(file);
    }
    if(files.disk.seen_missing(file)) {
        return true;
    }
    auto seen = files.disk.seen_hash(file);
    return seen && *seen != content_hash;
}

IndexQuery::IndexQuery(const ProjectIndex& index, FileTable& files, const FreshnessGate* gate) :
    index(index), files(files), gate(gate) {}

const OpenBuffer* IndexQuery::buffer_of(Fid file) const {
    auto it = ranges::find(open, file, &OpenBuffer::file);
    return it == open.end() ? nullptr : &*it;
}

const Shard* IndexQuery::buffer_shard(const OpenBuffer& buffer) const {
    if(!buffer.index || !buffer.index->loaded()) {
        return nullptr;
    }
    // A shard compiled from exactly the buffer's bytes describes it however
    // its dependencies moved since (the shard's DepsOnly contract).
    auto& state = *buffer.index;
    auto& shard = state.shard_of(state.focus_file());
    if(buffer.current || shard.matches_content(buffer.text.size(), buffer.hash)) {
        return buffer.merged ? buffer.merged.get() : &shard;
    }
    return nullptr;
}

IndexSource IndexQuery::buffer_source(IndexSource::Kind kind,
                                      const OpenBuffer& buffer,
                                      const Shard& shard) const {
    IndexSource source{
        .kind = kind,
        .file = buffer.file,
        .path = files.display(buffer.file),
        .shard = &shard,
        .positions = buffer.positions,
    };
    // The buffer's own compile resolved what it instantiates and expands;
    // what the other units do in the file serves while the buffer is the
    // text they read.
    if(kind == IndexSource::Kind::SessionShard) {
        auto& state = *buffer.index;
        if(auto section = state.instantiation_section_of(state.focus_file())) {
            source.instantiations.push_back(&state.instantiation_shard(*section));
        }
    }
    auto* stored = index.instantiation_shard(buffer.file);
    if(kind != IndexSource::Kind::PreambleShard && stored &&
       stored->content_hash() == buffer.hash) {
        source.instantiations.push_back(stored);
    }
    return source;
}

void IndexQuery::for_each_session_index(llvm::function_ref<bool(const TUIndex&)> visit) const {
    for(auto& buffer: open) {
        if(buffer_shard(buffer) && !visit(*buffer.index)) {
            return;
        }
    }
}

void IndexQuery::for_each_preamble(llvm::function_ref<bool(const IndexSource&)> visit) const {
    for(auto& buffer: open) {
        auto& state = buffer.preamble;
        if(!state) {
            continue;
        }
        // The preamble shard's entries are buffer offsets of the file that
        // built the blob: serve them only for that very file and only while
        // the buffer still starts with the exact preamble text the blob was
        // built from (OpenBuffer::preamble_current) — body edits never move
        // preamble entries, so no dirty-flag gating is needed on top.
        auto last = state->focus_file();
        if(files.find(Spelling::absolute(state->path(last))) != buffer.file ||
           !buffer.preamble_current) {
            continue;
        }
        if(!visit(buffer_source(IndexSource::Kind::PreambleShard, buffer, state->shard_of(last)))) {
            return;
        }
    }
}

void IndexQuery::for_each_overlay(llvm::function_ref<bool(const TUIndex&)> visit) const {
    // Buffers with identical preambles share one blob; visit it once. The
    // build's precompiled header a preamble's PCH is built over has none:
    // what it holds is indexed with the unit writing it.
    llvm::SmallPtrSet<const TUIndex*, 4> seen;
    for(auto& buffer: open) {
        auto* state = buffer.preamble.get();
        if(state && seen.insert(state).second && !visit(*state)) {
            return;
        }
    }
}

std::vector<Fid> IndexQuery::outdated_files() const {
    std::vector<Fid> outdated;
    llvm::DenseSet<Fid> recorded;
    index.for_each_indexed_file([&](Fid file, std::uint64_t content) {
        recorded.insert(file);
        if(buffer_of(file) ? !serving(file) : gate && gate->moved_on(file, content)) {
            outdated.push_back(file);
        }
    });
    // A buffer of a file the disk index never held, edited since its
    // compile indexed it.
    for(auto& buffer: open) {
        if(!recorded.contains(buffer.file) && buffer.index && !serving(buffer.file)) {
            outdated.push_back(buffer.file);
        }
    }
    return outdated;
}

std::optional<IndexSource> IndexQuery::serving(Fid file) const {
    if(auto* buffer = buffer_of(file)) {
        if(auto* shard = buffer_shard(*buffer)) {
            return buffer_source(IndexSource::Kind::SessionShard, *buffer, *shard);
        }
        auto* shard = index.shard(file);
        if(!shard || !shard->matches_content(buffer->text.size(), buffer->hash)) {
            return std::nullopt;
        }
        return buffer_source(IndexSource::Kind::Shard, *buffer, *shard);
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

bool IndexQuery::with_preamble(const IndexSource& source,
                               llvm::function_ref<void(const Shard&)> visit) const {
    if(source.kind != IndexSource::Kind::SessionShard) {
        return false;
    }
    bool found = false;
    for_each_preamble([&](const IndexSource& preamble) {
        if(preamble.file != source.file) {
            return true;
        }
        visit(*preamble.shard);
        found = true;
        return false;
    });
    return found;
}

const InstantiationShard* IndexQuery::instantiations_of(Fid file, const Shard& shard) const {
    if(!with_instantiations) {
        return nullptr;
    }
    auto* instantiations = index.instantiation_shard(file);
    return instantiations && instantiations->content_hash() == shard.content_hash() ? instantiations
                                                                                    : nullptr;
}

std::shared_ptr<TUIndex> IndexQuery::preamble_index(Fid file) const {
    auto* buffer = buffer_of(file);
    return buffer && buffer->preamble_current ? buffer->preamble : nullptr;
}

bool IndexQuery::visit_overlay_relations(const TUIndex& state,
                                         SymbolHash hash,
                                         RelationKind kind,
                                         RelationVisitor visitor) const {
    auto focus_id = state.focus_file();
    bool stopped = false;
    // What the TU's instantiations resolve in its other files, in the
    // coordinates of the text the TU read: the overlay's own entries of
    // the file, or the disk shard of that very text.
    llvm::SmallVector<std::uint32_t, 4> instantiated_sections;
    if(with_instantiations) {
        instantiated_sections = state.instantiation_sections_naming(hash);
    }
    for(auto i: instantiated_sections) {
        if(stopped) {
            break;
        }
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
        if(!shard || shard->content_hash() != instantiations.content_hash() || buffer_of(file) ||
           (gate && gate->stale(file, shard->content_hash()))) {
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
    for(auto i: state.sections_naming(hash)) {
        if(stopped) {
            break;
        }
        auto local_id = state.section_path(i);
        if(local_id == focus_id) {
            continue;
        }
        auto& shard = state.section_shard(i);
        std::optional<IndexSource> source;
        shard.relations_of(hash, kind, [&](const Relation& relation) {
            // Most sections naming the symbol hold no relation of `kind`:
            // the file is resolved once one turns up.
            if(!source) {
                auto file = files.intern(Spelling::absolute(state.path(local_id)));
                if(buffer_of(file) || (gate && gate->stale(file, shard.content_hash()))) {
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
                                    RelationKind kind,
                                    llvm::function_ref<void(Fid)> visit) const {
    auto which = kind.is_one_of(RelationKind::Definition)    ? SymbolFiles::Defining
                 : kind.is_one_of(RelationKind::Declaration) ? SymbolFiles::Declaring
                                                             : SymbolFiles::Naming;
    index.for_each_file(hash, which, visit);
    if(index.find_symbol(hash) || !anchor.valid()) {
        return;
    }
    // The anchor holds an entry of a symbol below external scope, whether
    // or not other files do.
    visit(anchor);
    // An open buffer's compile names the files declaring the locals it reads
    // through a precompiled header or a module, whose builds indexed them:
    // the symbol has no record listing them until the buffer's own unit is
    // indexed.
    if(buffer_of(anchor)) {
        for_each_session_index([&](const TUIndex& state) {
            for(auto path_id: state.paths_naming(hash, false)) {
                visit(files.intern(Spelling::absolute(state.path(path_id))));
            }
            return true;
        });
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

    // TODO: each reference file is resolved (stale check, display path,
    // positions, instantiation shard) before its shard is asked for a relation
    // of `kind`: narrow kinds on widely referenced symbols pay that per file.
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
        for_each_disk_file(hash, anchor, kind, visit);
        if(with_instantiations) {
            index.for_each_file(hash, SymbolFiles::Instantiating, visit);
        }
        return completed;
    };
    auto sessions = [&] {
        if(!mask.session) {
            return true;
        }
        for(auto& buffer: open) {
            auto* shard = buffer_shard(buffer);
            if(shard && !emit(buffer_source(IndexSource::Kind::SessionShard, buffer, *shard))) {
                return false;
            }
        }
        return true;
    };
    auto preambles = [&] {
        if(!mask.preamble) {
            return true;
        }
        for_each_preamble(emit);
        return !stopped;
    };
    auto overlays = [&] {
        if(!mask.overlay) {
            return true;
        }
        auto visit = [&](const TUIndex& state) {
            stopped = !visit_overlay_relations(state, hash, kind, visitor);
            return !stopped;
        };
        for_each_overlay(visit);
        if(!stopped) {
            for_each_session_index(visit);
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
    return buffer_of(file) ? index.shard(file) : nullptr;
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
    // The record follows the stored shard, which an open buffer edited
    // since, or a variant no unit contributes any more, may not serve.
    Fid file{identity->file};
    auto* shard = declaring_shard(file);
    return shard && shard->has_relation(hash, RelationKind::Definition) ? file : Fid{};
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
    std::string kept;
    auto separates = [&](const Named& named) {
        if(named.range.begin != offset || named.range.length() > 2 ||
           ranges::any_of(named.symbols, [&](SymbolHash hash) {
               auto info = symbol_info(hash, file);
               return info && name_form(info->flags) == NameForm::Operator;
           })) {
            return false;
        }
        auto text = source_text(*source, kept);
        return text && ranges::contains(std::array{":", "=", "&&", "||", "?"},
                                        text->slice(named.range.begin, named.range.end));
    };
    auto hit = [&](const Shard& shard) {
        auto named = named_at(shard, offset, is_macro);
        if(with_instantiations && &shard == source->shard) {
            for(auto* instantiations: source->instantiations) {
                add_resolved(*instantiations, offset, true, named);
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
    // The preamble region's occurrences (macro definitions and references
    // before the bound) are served only from a preamble index built from
    // this very file that still describes its start (preamble drift,
    // shared-PCH identity).
    with_preamble(*source, [&](const Shard& preamble) { hit(preamble); });
    return cursor;
}

std::optional<IndexQuery::Cursor> IndexQuery::instantiated_at(Fid file,
                                                              std::uint32_t offset) const {
    auto source = serving(file);
    if(!source || !with_instantiations) {
        return std::nullopt;
    }
    std::optional<Named> named;
    for(auto* instantiations: source->instantiations) {
        add_resolved(*instantiations, offset, false, named);
    }
    if(!named) {
        return std::nullopt;
    }
    auto site = source->site(named->range);
    if(!site) {
        return std::nullopt;
    }
    return Cursor{.symbols = std::move(named->symbols), .site = *site};
}

bool IndexQuery::relates_at(Fid file,
                            LocalSourceRange range,
                            SymbolHash symbol,
                            RelationKind kind) const {
    auto source = serving(file);
    if(!source) {
        return false;
    }
    bool found = false;
    auto hit = [&](const Shard& shard) {
        shard.relations_of(symbol, kind, [&](const Relation& relation) {
            found = relation.range == range;
            return !found;
        });
        return found;
    };
    if(!hit(*source->shard)) {
        with_preamble(*source, [&](const Shard& preamble) { hit(preamble); });
    }
    return found;
}

std::optional<IndexQuery::Cursor> IndexQuery::symbol_at(Fid file,
                                                        protocol::Position position,
                                                        PositionEncoding encoding) const {
    auto source = serving(file);
    if(!source) {
        return std::nullopt;
    }
    auto offset = source->positions.offset(position, encoding);
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
    for_each_session_index([&](const TUIndex& state) {
        if(auto identity = state.find_symbol(hash)) {
            adopt(*identity);
        }
        return !found;
    });
    if(found) {
        return found;
    }

    if(auto identity = index.find_symbol(hash)) {
        return SymbolInfo::from(hash, *identity);
    }

    // A symbol that exists only under an open buffer's context (or in
    // headers no disk TU has been indexed with) is in no disk table.
    for_each_overlay([&](const TUIndex& state) {
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
    if(auto* shard = anchor.valid() ? index.shard(anchor) : nullptr) {
        if(auto identity = shard->find_symbol(hash)) {
            return SymbolInfo::from(hash, *identity);
        }
    }
    // An instantiation or an expansion names it in a file other than its
    // own: the files holding its entries hold its declaration.
    index.for_each_file(hash, SymbolFiles::Naming, [&](Fid file) {
        auto* shard = found ? nullptr : index.shard(file);
        if(auto identity = shard ? shard->find_symbol(hash) : std::nullopt) {
            adopt(*identity);
        }
    });
    return found;
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

std::vector<IndexQuery::Placed> IndexQuery::definitions(SymbolHash hash, Fid anchor) const {
    std::vector<Placed> result;
    for_each_relation(
        hash,
        anchor,
        RelationKind::Definition,
        Order::DiskFirst,
        {},
        [&](const IndexSource& source, const Relation& relation) {
            if(auto site = source.site(relation.range)) {
                result.push_back(
                    {.site = *site, .extent = extent_of(source, relation).value_or(*site)});
            }
            return true;
        });
    ranges::sort(result, [](const Placed& lhs, const Placed& rhs) {
        return std::tuple(site_key(lhs.site), lhs.site.origin) <
               std::tuple(site_key(rhs.site), rhs.site.origin);
    });
    auto [first, last] = ranges::unique(result, [](const Placed& lhs, const Placed& rhs) {
        return site_key(lhs.site) == site_key(rhs.site);
    });
    result.erase(first, last);
    return result;
}

std::optional<IndexQuery::Placed> IndexQuery::canonical_placed(SymbolHash hash, Fid anchor) const {
    if(auto placed = first_placed(hash, anchor, RelationKind::Definition)) {
        return placed;
    }
    // A declaration stands in only for a symbol nothing defines. A
    // definition may sit in a file whose shard is not serving — withheld
    // as stale, or an open buffer that moved on from it — and then stays
    // unavailable (`clice query` reports such a symbol as not found). A
    // deleted file holds nothing anymore.
    bool unavailable = false;
    for_each_disk_file(hash, anchor, RelationKind::Definition, [&](Fid file) {
        auto* shard = index.shard(file);
        if(unavailable || !shard || serving(file) || files.disk.seen_missing(file)) {
            return;
        }
        unavailable = shard->has_relation(hash, RelationKind::Definition);
    });
    if(unavailable) {
        return std::nullopt;
    }
    return first_placed(hash, anchor, RelationKind::Declaration);
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
                              result.push_back({.symbol = relation.target, .anchor = source.file});
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

std::optional<llvm::StringRef> IndexQuery::source_text(const IndexSource& source,
                                                       std::string& kept) const {
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
    std::string kept;
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
