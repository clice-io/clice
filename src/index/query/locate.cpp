module clice;

import :command.file_kind;
import :index.query;
import :index.query.locate;
import :index.query.site;
import :index.search;
import :index.indexing.tu_index;
import :semantic.identity;
import :semantic.symbol;
import :semantic.token_class;
import :support.logging;

namespace clice::index {

namespace {

/// A search's ranked symbols before their sites are resolved, and
/// whether the limit cut the ranking short.
struct Ranked {
    NameRank rank;
    SymbolInfo symbol;
    /// The file a file's own symbol was found in, which holds its
    /// entries; invalid for a project symbol.
    Fid anchor;
};

bool ranked_before(const Ranked& lhs, const Ranked& rhs) {
    return ranks_after(rhs.rank,
                       rhs.symbol.name,
                       rhs.symbol.args,
                       rhs.symbol.hash,
                       lhs.rank,
                       lhs.symbol.name,
                       lhs.symbol.args,
                       lhs.symbol.hash);
}

struct RankedHits {
    std::vector<Ranked> hits;
    bool exhausted = true;
};

RankedHits ranked_search(const IndexQuery& index, const Locator& query, std::uint32_t limit) {
    const auto& project = index.project();
    auto& files = index.file_table();
    std::vector<Ranked> hits;
    llvm::DenseSet<SymbolHash> seen;
    auto indexed = project.search_index().search(query, limit, [&](std::uint32_t path) {
        auto file = project.fid_of(path);
        return file.valid() ? files.resolve(file) : llvm::StringRef();
    });
    // A damaged index answers incompletely, a missing one not at all:
    // until a full build every symbol record is judged one by one instead.
    bool scan_records = project.search_index().damaged() || !project.search_index().loaded();
    bool exhausted = scan_records || indexed.exhausted;
    if(!scan_records) {
        for(auto& hit: indexed.hits) {
            // A symbol whose record changed since the newest run's snapshot is
            // read from its record below, not from the index's stale copy.
            if(project.search_pending().contains(hit.hash)) {
                continue;
            }
            Fid anchor;
            if(hit.file != no_file) {
                anchor = project.fid_of(hit.file);
                // Read from the file's shard below.
                if(project.search_pending_files().contains(anchor)) {
                    continue;
                }
            }
            auto info = index.symbol_info(hit.hash, anchor);
            if(info && seen.insert(hit.hash).second) {
                hits.push_back({.rank = hit.rank, .symbol = std::move(*info), .anchor = anchor});
            }
        }
    }

    // What the index does not file — the symbols merged since its newest
    // run's snapshot, and the open sessions' — is judged symbol by symbol
    // against the same query.
    NameRanker ranker(query);
    bool scoped = query.absolute || !query.scope.empty() || query.mode == Locator::Mode::Members;
    // Candidates share their containers: each is looked up once per
    // search, not once per candidate through every open session's table.
    llvm::DenseMap<SymbolHash, std::optional<SymbolInfo>> scopes;
    auto scope_info = [&](SymbolHash scope, Fid anchor) {
        auto [it, inserted] = scopes.try_emplace(scope);
        if(inserted) {
            it->second = index.symbol_info(scope, anchor);
        }
        return it->second;
    };
    // `declared`: the files declaring the symbol, the one file of a symbol
    // below external scope.
    auto consider = [&](SymbolHash hash,
                        const SymbolView& identity,
                        llvm::ArrayRef<llvm::StringRef> declared,
                        std::uint32_t reference_files,
                        Fid anchor) {
        if(!searchable(identity.kind, identity.name, identity.flags) || seen.contains(hash) ||
           (query.nameable_only && identity.scope != SymbolScope::External &&
            ranges::none_of(declared, is_header_path))) {
            return;
        }
        if(!query.kinds.empty() && !ranges::contains(query.kinds, identity.kind)) {
            return;
        }
        if(!query.paths.empty() && ranges::none_of(declared, [&](llvm::StringRef path) {
               return ranges::any_of(query.paths, [&](const std::string& wanted) {
                   return path_matches(wanted, path);
               });
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
            auto containers = scope_chain(hash, identity.parent, [&](SymbolHash scope) {
                return scope_info(scope, anchor);
            });
            llvm::SmallVector<ScopeEntry, 4> chain;
            for(auto& container: containers) {
                chain.push_back({.name = container.name, .args = container.args});
            }
            // A qualified lookup finds an enumerator in its enumeration's
            // scope too.
            if(query.qualified_lookup && !containers.empty() &&
               containers.back().kind == SymbolKind::Enum) {
                chain.pop_back();
            }
            if(!in_scope(query, chain)) {
                return;
            }
        }
        seen.insert(hash);
        hits.push_back(
            {.rank = *rank, .symbol = SymbolInfo::from(hash, identity), .anchor = anchor});
    };
    auto references_of = [&](SymbolHash hash) -> std::uint32_t {
        return project.reference_count(hash);
    };
    auto consider_symbol =
        [&](SymbolHash hash, const SymbolView& symbol, std::uint32_t references) {
            llvm::SmallVector<llvm::StringRef, 4> declared;
            if(!query.paths.empty()) {
                project.for_each_file(hash, SymbolFiles::Declaring, [&](Fid file) {
                    declared.push_back(files.resolve(file));
                });
            }
            consider(hash, symbol, declared, references, {});
        };
    if(scan_records) {
        project.for_each_symbol(
            [&](SymbolHash hash, const SymbolView& symbol, std::uint32_t references) {
                consider_symbol(hash, symbol, references);
                return true;
            });
    } else {
        for(auto hash: project.search_pending()) {
            if(auto symbol = project.find_symbol(hash)) {
                consider_symbol(hash, *symbol, project.reference_count(hash));
            }
        }
    }
    // The names of internal linkage of the files merged since the newest
    // run's snapshot live in their shards alone.
    for(auto file: project.search_pending_files()) {
        llvm::StringRef path = files.resolve(file);
        if(query.nameable_only && !is_header_path(path)) {
            continue;
        }
        if(auto* shard = index.declaring_shard(file)) {
            shard->for_each_local_symbol([&](SymbolHash hash, const SymbolView& identity) {
                if(identity.scope == SymbolScope::Internal && shard->declares(hash)) {
                    consider(hash, identity, {path}, 0, file);
                }
                return true;
            });
        }
    }
    index.for_each_session_index([&](const TUIndex& state) {
        state.for_each_symbol(
            [&](SymbolHash hash, const SymbolView& identity, llvm::ArrayRef<std::uint32_t>) {
                // The index judged the symbols it holds already, under
                // the same name.
                if(!scan_records && !project.search_pending().contains(hash) &&
                   project.find_symbol(hash)) {
                    return true;
                }
                llvm::SmallVector<llvm::StringRef, 1> declared;
                if(identity.file != no_file) {
                    declared.push_back(state.path(identity.file));
                }
                consider(hash, identity, declared, references_of(hash), {});
                return true;
            });
        return true;
    });
    // A file's own symbols — locals, parameters, internal-linkage
    // entities — are in no record but the file's shard, which holds every
    // such symbol it names: a query naming files looks for the ones each
    // of them declares.
    if(!query.paths.empty()) {
        // The indexed files the database names, and the ones this process
        // met (an open buffer's file among them).
        llvm::SetVector<Fid> candidates;
        project.for_each_indexed_file([&](Fid file, std::uint64_t) { candidates.insert(file); });
        for(std::uint32_t raw = 0; raw < files.spellings.size(); raw += 1) {
            candidates.insert(Fid{raw});
        }
        for(auto file: candidates) {
            llvm::StringRef path = files.resolve(file);
            if(ranges::none_of(query.paths, [&](const std::string& wanted) {
                   return path_matches(wanted, path);
               })) {
                continue;
            }
            auto* shard = index.declaring_shard(file);
            if(!shard) {
                continue;
            }
            shard->for_each_local_symbol([&](SymbolHash hash, const SymbolView& identity) {
                if(shard->declares(hash)) {
                    consider(hash, identity, {path}, 0, file);
                }
                return true;
            });
        }
    }

    ranges::sort(hits, ranked_before);
    if(hits.size() > limit) {
        hits.resize(limit);
        exhausted = false;
    }
    return {.hits = std::move(hits), .exhausted = exhausted};
}

}  // namespace

std::optional<Site> predefined_site(const SymbolInfo& symbol) {
    if(symbol.kind != SymbolKind::Macro) {
        return std::nullopt;
    }
    for(auto [command_line, name]:
        {std::pair(true, "<command line>"), std::pair(false, "<built-in>")}) {
        if(symbol.hash == EntityTable::predefined_macro(symbol.name, command_line)) {
            return Site{.path = name};
        }
    }
    return std::nullopt;
}

std::vector<IndexQuery::Located> every_definition(const IndexQuery& index,
                                                  std::vector<IndexQuery::Located> symbols) {
    std::vector<IndexQuery::Located> result;
    for(auto& located: symbols) {
        auto definitions = index.definitions(located.symbol.hash, located.site.file);
        if(definitions.size() < 2) {
            result.push_back(std::move(located));
            continue;
        }
        for(auto& definition: definitions) {
            result.push_back({.symbol = located.symbol,
                              .site = std::move(definition.site),
                              .extent = std::move(definition.extent)});
        }
    }
    return result;
}

std::vector<IndexQuery::Located> search(const IndexQuery& index,
                                        const Locator& query,
                                        std::uint32_t limit) {
    Timer timer;
    if(limit == 0 || !query.by_pattern()) {
        return {};
    }
    // A symbol no source places cedes its slot to the next one: the
    // ranking is fetched wider until the cut is filled or exhausted.
    std::vector<IndexQuery::Located> results;
    for(std::uint32_t fetch = limit * 2;; fetch *= 4) {
        auto ranked = ranked_search(index, query, fetch);
        results.clear();
        for(auto& hit: ranked.hits) {
            if(results.size() == limit) {
                break;
            }
            if(auto placed = index.canonical_placed(hit.symbol.hash, hit.anchor)) {
                results.push_back({.symbol = std::move(hit.symbol),
                                   .site = placed->site,
                                   .extent = placed->extent});
            } else if(auto site = predefined_site(hit.symbol)) {
                results.push_back(
                    {.symbol = std::move(hit.symbol), .site = *site, .extent = *site});
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
             timer.precise_ms());
    return results;
}

Completables completables(const IndexQuery& index,
                          llvm::StringRef prefix,
                          std::uint32_t limit,
                          llvm::ArrayRef<llvm::StringRef> scope,
                          bool absolute) {
    std::vector<Locator::Segment> segments;
    for(auto segment: scope) {
        segments.push_back({.name = segment.str()});
    }
    // Of the kinds a namespace's names have: a class's members, which a
    // scope's subtree holds more of, would crowd them out of the ranking.
    Locator query{
        .scope = std::move(segments),
        .absolute = absolute,
        .pattern = prefix.str(),
        .kinds = {SymbolKind::Macro,
                  SymbolKind::Namespace,
                  SymbolKind::Class,
                  SymbolKind::Struct,
                  SymbolKind::Union,
                  SymbolKind::Enum,
                  SymbolKind::Type,
                  SymbolKind::EnumMember,
                  SymbolKind::Function,
                  SymbolKind::Variable,
                  SymbolKind::Concept},
        .nameable_only = true,
        .qualified_lookup = absolute || !scope.empty(),
    };
    // The names completion does not offer (a class's members) take places
    // in the ranking: it is fetched wider than the names wanted.
    auto ranked = ranked_search(index, query, limit * 8);
    Completables result{.exhausted = ranked.exhausted};
    /// The names taken, spelled through their scopes: the limit counts a
    /// name once however many overloads and specializations it has.
    llvm::StringSet<> taken;
    /// Their names alone, past the limit the only ones more of whose
    /// overloads join.
    llvm::StringSet<> taken_names;
    for(auto& hit: ranked.hits) {
        if(!has_flag(hit.symbol.flags, SymbolFlags::Completable)) {
            continue;
        }
        if(taken.size() == limit && !taken_names.contains(hit.symbol.name)) {
            result.exhausted = false;
            continue;
        }
        // A header's name of internal linkage is in its file alone.
        Fid declared = hit.anchor;
        if(auto identity = index.project().find_symbol(hit.symbol.hash);
           identity && identity->file != no_file) {
            declared = Fid{identity->file};
        }
        auto chain = index.container_chain(hit.symbol.hash, declared);
        std::string spelled;
        for(auto& container: chain) {
            spelled += container.name;
            spelled += "::";
        }
        spelled += hit.symbol.name;
        if(!taken.contains(spelled)) {
            if(taken.size() == limit) {
                result.exhausted = false;
                continue;
            }
            taken.insert(spelled);
            taken_names.insert(hit.symbol.name);
        }
        result.names.push_back(
            {.symbol = std::move(hit.symbol), .scope = std::move(chain), .file = declared});
    }
    return result;
}

std::optional<IndexQuery::Cursor> cursor_at(const IndexQuery& index,
                                            const Locator::Position& place) {
    assert(place.column);
    auto file = index.file_table().intern(Spelling::absolute(place.path));
    // A stale index describes text that no longer exists: resolving the
    // requested place against it would name the wrong symbol.
    auto source = index.serving(file);
    if(!source) {
        return std::nullopt;
    }
    auto bounds = source->positions.line_bounds(static_cast<std::uint32_t>(place.line - 1));
    if(!bounds) {
        return std::nullopt;
    }
    // The column counts bytes: the line's start plus the column, bounded
    // by the line's own length.
    auto offset = bounds->begin + static_cast<std::uint32_t>(*place.column - 1);
    if(offset > bounds->end) {
        return std::nullopt;
    }
    return index.symbol_at(file, offset);
}

/// The ranking of `query` down to every symbol spelling the name exactly,
/// however many.
static RankedHits exact_search(const IndexQuery& index, const Locator& query) {
    for(std::uint32_t limit = 64;; limit *= 4) {
        auto ranked = ranked_search(index, query, limit);
        if(ranked.exhausted || (!ranked.hits.empty() && ranked.hits.back().rank.tier > 1)) {
            return ranked;
        }
    }
}

std::vector<IndexQuery::Located> locate(const IndexQuery& index, const Locator& query) {
    if(query.handle) {
        Fid anchor;
        if(!query.paths.empty()) {
            anchor = index.file_table().intern(Spelling::absolute(query.paths.front()));
        }
        if(auto located = index.resolve(*query.handle, anchor)) {
            return {std::move(*located)};
        }
        return {};
    }

    if(query.position && query.position->column) {
        auto cursor = cursor_at(index, *query.position);
        return cursor ? index.resolve_at(*cursor) : std::vector<IndexQuery::Located>{};
    }
    if(query.position) {
        auto& place = *query.position;
        auto file = index.file_table().intern(Spelling::absolute(place.path));
        auto source = index.serving(file);
        if(!source) {
            return {};
        }
        auto bounds = source->positions.line_bounds(static_cast<std::uint32_t>(place.line - 1));
        if(!bounds) {
            return {};
        }
        // The definitions on the line, the file's own symbols included:
        // the serving source holds every entry, whichever table names it.
        std::vector<IndexQuery::Located> defined;
        llvm::DenseSet<SymbolHash> seen;
        source->shard->for_each_relation([&](SymbolHash hash, const Relation& r) {
            if(r.kind != RelationKind::Definition || !bounds->contains(r.range.begin) ||
               !seen.insert(hash).second) {
                return true;
            }
            auto site = source->site(r.range);
            if(!site) {
                return true;
            }
            if(auto info = index.symbol_info(hash, file)) {
                defined.push_back({.symbol = std::move(*info),
                                   .site = *site,
                                   .extent = extent_of(*source, r).value_or(*site)});
            }
            return true;
        });
        return defined;
    }

    auto ranked = exact_search(index, query);
    // Spelling the name exactly, case included, decides it; otherwise every
    // match stands. A class and its constructors spell one name: asked by
    // it, the class answers. So does a template asked by its bare name: a
    // specialization is named with its arguments.
    llvm::DenseSet<SymbolHash> exact;
    for(auto& hit: ranked.hits) {
        if(hit.rank.tier == 0) {
            exact.insert(hit.symbol.hash);
        }
    }
    std::vector<IndexQuery::Located> results;
    for(auto& hit: ranked.hits) {
        if(!exact.empty() && hit.rank.tier > 0) {
            continue;
        }
        if(name_form(hit.symbol.flags) == NameForm::Constructor &&
           exact.contains(hit.symbol.parent)) {
            continue;
        }
        if(query.args.empty() && has_flag(hit.symbol.flags, SymbolFlags::Specialization) &&
           ranges::any_of(
               index.targets(hit.symbol.hash, hit.anchor, RelationKind::Primary),
               [&](const IndexQuery::Target& primary) { return exact.contains(primary.symbol); })) {
            continue;
        }
        if(auto placed = index.canonical_placed(hit.symbol.hash, hit.anchor)) {
            results.push_back(
                {.symbol = std::move(hit.symbol), .site = placed->site, .extent = placed->extent});
        }
    }
    return results;
}

std::vector<IndexQuery::Located> predefined_macros(const IndexQuery& index, const Locator& query) {
    std::vector<IndexQuery::Located> macros;
    auto add = [&](std::optional<SymbolInfo> symbol) {
        if(auto site = symbol ? predefined_site(*symbol) : std::nullopt) {
            macros.push_back({.symbol = std::move(*symbol), .site = *site, .extent = *site});
        }
    };
    if(query.handle) {
        add(index.symbol_info(*query.handle));
    } else if(query.position && query.position->column) {
        if(auto cursor = cursor_at(index, *query.position)) {
            for(auto symbol: cursor->symbols) {
                add(index.symbol_info(symbol, cursor->site.file));
            }
        }
    } else if(query.by_pattern()) {
        for(auto& hit: exact_search(index, query).hits) {
            if(hit.rank.tier == 0) {
                add(std::move(hit.symbol));
            }
        }
    }
    return macros;
}

std::vector<IndexQuery::Target> reached_by_selector(const IndexQuery& index,
                                                    const SymbolInfo& method) {
    auto ranked = exact_search(
        index,
        {.mode = Locator::Mode::Exact, .pattern = method.name, .kinds = {SymbolKind::Method}});
    std::vector<IndexQuery::Target> reached;
    for(auto& hit: ranked.hits) {
        auto& other = hit.symbol;
        if(other.hash != method.hash && name_form(other.flags) == NameForm::Selector &&
           other.token_class.has(SemanticTokenModifier::Static) ==
               method.token_class.has(SemanticTokenModifier::Static)) {
            reached.push_back({.symbol = other.hash, .anchor = hit.anchor});
        }
    }
    return reached;
}

}  // namespace clice::index
