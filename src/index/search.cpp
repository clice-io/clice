module clice;

import :index.query;
import :index.search;
import :index.search_index;
import :support.logging;
import :support.timer;

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

RankedHits ranked_search(const IndexQuery& index, const SymbolQuery& query, std::uint32_t limit) {
    const auto& project = index.project();
    auto& files = index.file_table();
    std::vector<Ranked> hits;
    llvm::DenseSet<SymbolHash> seen;
    auto indexed = project.search_index().search(query, limit, [&](std::uint32_t path) {
        auto file = project.fid_of(path);
        return file.valid() && ranges::any_of(query.paths, [&](const std::string& wanted) {
                   return path_matches(wanted, files.resolve(file));
               });
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
    bool scoped =
        query.absolute || !query.scope.empty() || query.mode == SymbolQuery::Mode::Members;
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
    auto consider = [&](SymbolHash hash,
                        const SymbolView& identity,
                        llvm::StringRef path,
                        std::uint32_t reference_files,
                        Fid anchor) {
        if(!searchable(identity.kind, identity.name, identity.flags) || seen.contains(hash) ||
           (query.external_only && identity.scope != SymbolScope::External)) {
            return;
        }
        if(!query.kinds.empty() && !ranges::contains(query.kinds, identity.kind)) {
            return;
        }
        if(!query.paths.empty() && ranges::none_of(query.paths, [&](const std::string& wanted) {
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
            auto containers = scope_chain(hash, identity.parent, [&](SymbolHash scope) {
                return scope_info(scope, anchor);
            });
            llvm::SmallVector<ScopeEntry, 4> chain;
            for(auto& container: containers) {
                chain.push_back({.name = container.name, .args = container.args});
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
            llvm::StringRef path;
            if(symbol.file != no_file) {
                path = files.resolve(Fid{symbol.file});
            }
            consider(hash, symbol, path, references, {});
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
    if(!query.external_only) {
        for(auto file: project.search_pending_files()) {
            auto* shard = index.declaring_shard(file);
            if(!shard) {
                continue;
            }
            llvm::StringRef path = files.resolve(file);
            shard->for_each_local_symbol([&](SymbolHash hash, const SymbolView& identity) {
                if(identity.scope == SymbolScope::Internal && shard->declares(hash)) {
                    consider(hash, identity, path, 0, file);
                }
                return true;
            });
        }
    }
    if(index.live_sources()) {
        index.live_sources()->for_each_session_index([&](const TUIndex& state) {
            state.for_each_symbol(
                [&](SymbolHash hash, const SymbolView& identity, llvm::ArrayRef<std::uint32_t>) {
                    llvm::StringRef path;
                    if(identity.file != no_file) {
                        path = state.path(identity.file);
                    }
                    consider(hash, identity, path, references_of(hash), {});
                    return true;
                });
            return true;
        });
    }
    // A file's own symbols — locals, parameters, internal-linkage
    // entities — are in no record but the file's shard, which holds every
    // such symbol it names: a query naming files looks for the ones each
    // of them declares.
    if(!query.paths.empty()) {
        // The indexed files the database names, and the ones this process
        // met (an open buffer's file among them).
        llvm::SetVector<Fid> candidates;
        project.for_each_indexed_file([&](Fid file) { candidates.insert(file); });
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
                    consider(hash, identity, path, 0, file);
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

std::vector<IndexQuery::Located> search(const IndexQuery& index,
                                        const SymbolQuery& query,
                                        std::uint32_t limit) {
    ScopedTimer timer;
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

Completables completables(const IndexQuery& index,
                          llvm::StringRef prefix,
                          const CompletionScope& scope,
                          Fid file,
                          std::uint32_t limit) {
    SymbolQuery scoped{.absolute = scope.absolute, .pattern = prefix.str()};
    for(auto& name: scope.qualifier) {
        scoped.scope.push_back({.name = name});
    }
    /// Whether a name the query found lies directly in a scope the
    /// qualifier names, as completion after it lists: the query takes the
    /// names below them too. An enumerator counts in its enumeration's
    /// scope.
    auto direct = [&](llvm::ArrayRef<SymbolInfo> chain) {
        if(!chain.empty() && chain.back().kind == SymbolKind::Enum) {
            chain = chain.drop_back();
        }
        if(chain.size() < scope.qualifier.size() ||
           (scope.absolute && chain.size() != scope.qualifier.size())) {
            return false;
        }
        return ranges::equal(chain.take_back(scope.qualifier.size()),
                             scope.qualifier,
                             {},
                             &SymbolInfo::name);
    };

    Completables result;
    llvm::DenseSet<SymbolHash> taken;
    /// Adds the best of the names `query` finds that `wanted` keeps, up to
    /// the limit.
    auto gather = [&](const SymbolQuery& query, auto wanted) {
        // Members outnumber the names of namespaces, and a qualifier's
        // scopes hold more below them: the ranking is fetched wider than
        // the names wanted.
        auto project_query = query;
        project_query.external_only = true;
        auto ranked = ranked_search(index, project_query, limit * 8);
        llvm::DenseSet<SymbolHash> seen;
        for(auto& hit: ranked.hits) {
            seen.insert(hit.symbol.hash);
        }
        /// A preamble's names, its own of internal linkage among them, are
        /// spelled through its own scopes.
        llvm::DenseMap<SymbolHash, llvm::SmallVector<SymbolInfo, 4>> preamble_scopes;
        index.for_each_pch_index(file, [&](const TUIndex& pch) {
            auto found = pch.names().search(query, limit * 8, [](std::uint32_t) { return false; });
            for(auto& hit: found.hits) {
                if(!seen.insert(hit.hash).second) {
                    continue;
                }
                if(auto identity = pch.find_symbol(hit.hash)) {
                    auto symbol = SymbolInfo::from(hit.hash, *identity);
                    preamble_scopes[hit.hash] =
                        scope_chain(hit.hash, symbol.parent, [&](SymbolHash parent) {
                            auto container = pch.find_symbol(parent);
                            return container ? std::optional(SymbolInfo::from(parent, *container))
                                             : std::nullopt;
                        });
                    ranked.hits.push_back(
                        {.rank = hit.rank, .symbol = std::move(symbol), .anchor = {}});
                }
            }
            ranked.exhausted = ranked.exhausted && found.exhausted;
        });
        ranges::sort(ranked.hits, ranked_before);

        result.exhausted = result.exhausted && ranked.exhausted;
        for(auto& hit: ranked.hits) {
            if(!has_flag(hit.symbol.flags, SymbolFlags::Completable) ||
               taken.contains(hit.symbol.hash)) {
                continue;
            }
            Fid declared;
            if(auto identity = index.project().find_symbol(hit.symbol.hash);
               identity && identity->file != no_file) {
                declared = Fid{identity->file};
            }
            auto preamble = preamble_scopes.find(hit.symbol.hash);
            auto chain = preamble != preamble_scopes.end()
                             ? std::move(preamble->second)
                             : index.container_chain(hit.symbol.hash, declared);
            if(!wanted(chain)) {
                continue;
            }
            if(result.names.size() == limit) {
                result.exhausted = false;
                break;
            }
            taken.insert(hit.symbol.hash);
            result.names.push_back(
                {.symbol = std::move(hit.symbol), .scope = std::move(chain), .file = declared});
        }
    };

    if(!scope.qualified()) {
        gather(scoped, [](llvm::ArrayRef<SymbolInfo>) { return true; });
        return result;
    }
    gather(scoped, direct);
    // A namespace's using-declarations and using-directives bring in the
    // names of others, which the index does not tell: the compile looks
    // these up in it as well.
    gather(SymbolQuery{.pattern = prefix.str()}, [](llvm::ArrayRef<SymbolInfo>) { return true; });
    return result;
}

std::vector<IndexQuery::Located> locate(const IndexQuery& index, const SymbolQuery& query) {
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

    if(query.position) {
        auto& place = *query.position;
        auto file = index.file_table().intern(Spelling::absolute(place.path));
        // A stale index describes text that no longer exists: resolving the
        // requested place against it would name the wrong symbol.
        auto source = index.serving(file);
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
            auto cursor = index.symbol_at(file, offset);
            if(!cursor) {
                return {};
            }
            return index.resolve_at(*cursor);
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

    // Every symbol spelling the name exactly is wanted, however many: the
    // ranking is fetched wider until a match of a lower tier ends it.
    RankedHits ranked;
    for(std::uint32_t limit = 64;; limit *= 4) {
        ranked = ranked_search(index, query, limit);
        if(ranked.exhausted || (!ranked.hits.empty() && ranked.hits.back().rank.tier > 1)) {
            break;
        }
    }
    // Spelling the name exactly decides it; otherwise every match stands.
    // A class and its constructors spell one name: asked by it, the class
    // answers. So does a template asked by its bare name: a specialization
    // is named with its arguments.
    llvm::DenseSet<SymbolHash> exact;
    for(auto& hit: ranked.hits) {
        if(hit.rank.tier <= 1) {
            exact.insert(hit.symbol.hash);
        }
    }
    std::vector<IndexQuery::Located> results;
    for(auto& hit: ranked.hits) {
        if(!exact.empty() && hit.rank.tier > 1) {
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

}  // namespace clice::index
