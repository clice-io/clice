module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

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

struct RankedHits {
    std::vector<Ranked> hits;
    bool exhausted = true;
};

RankedHits ranked_search(const IndexQuery& index, const SymbolQuery& query,
                                                 std::size_t limit) {
    const auto& project = index.project();
    auto& files = index.file_table();
    std::vector<Ranked> hits;
    llvm::DenseSet<SymbolHash> seen;
    auto indexed = project.search_index.search(query, limit, project.workspace);
    // A damaged index answers incompletely, a missing one not at all:
    // until the rebuild the whole table is judged symbol by symbol instead.
    bool scan_table = project.search_index.damaged() || !project.search_index.loaded();
    bool exhausted = scan_table || indexed.exhausted;
    if(!scan_table) {
        for(auto& hit: indexed.hits) {
            // A symbol the table changed since the index was built is read
            // from the table below, not from the index's stale copy.
            if(project.search_pending.contains(hit.hash)) {
                continue;
            }
            Fid anchor;
            if(!hit.file.empty()) {
                anchor = files.find(project.local(hit.file)).value_or(Fid{});
                // Read from the file's shard below.
                if(project.search_pending_files.contains(anchor)) {
                    continue;
                }
            }
            auto info = index.symbol_info(hit.hash, anchor);
            if(info && seen.insert(hit.hash).second) {
                hits.push_back({.rank = hit.rank, .symbol = std::move(*info), .anchor = anchor});
            }
        }
    }

    // What the index does not file — the symbols merged since it was
    // built, and the open sessions' — is judged symbol by symbol against the
    // same query.
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
        if(!is_searchable_kind(identity.kind) || identity.name.empty() ||
           has_flag(identity.flags, SymbolFlags::Unnamed) || seen.contains(hash) ||
           (query.external_only && identity.scope != SymbolScope::External)) {
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
    if(scan_table) {
        project.for_each_symbol(
            [&](SymbolHash hash, const SymbolView& symbol, std::uint32_t references) {
                consider_symbol(hash, symbol, references);
                return true;
            });
    } else {
        for(auto hash: project.search_pending) {
            if(auto symbol = project.find_symbol(hash)) {
                consider_symbol(hash, *symbol, project.reference_count(hash));
            }
        }
    }
    // The names of internal linkage of the files merged since the build
    // live in their shards alone.
    if(!query.external_only) {
        for(auto file: project.search_pending_files) {
            auto* shard = index.declaring_shard(file);
            if(!shard) {
                continue;
            }
            llvm::StringRef path = files.resolve(file);
            shard->for_each_local_symbol([&](SymbolHash hash, const SymbolView& identity) {
                if(identity.scope == SymbolScope::Internal && declares(*shard, hash)) {
                    consider(hash, identity, path, 0, file);
                }
                return true;
            });
        }
    }
    if(index.live_sources()) {
        index.live_sources()->each_session_index([&](const TUIndex& state) {
            state.iterate_symbols([&](SymbolHash hash,
                                      const SymbolView& identity,
                                      llvm::ArrayRef<std::uint32_t>) {
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
    // entities — are in no table but the file's shard, which holds every
    // such symbol it names: a query naming files looks for the ones each
    // of them declares.
    if(!query.paths.empty()) {
        for(std::uint32_t raw = 0; raw < files.spellings.size(); raw += 1) {
            Fid file{raw};
            llvm::StringRef path = files.resolve(file);
            if(llvm::none_of(query.paths, [&](const std::string& wanted) {
                   return path_matches(wanted, path);
               })) {
                continue;
            }
            auto* shard = index.declaring_shard(file);
            if(!shard) {
                continue;
            }
            shard->for_each_local_symbol([&](SymbolHash hash, const SymbolView& identity) {
                if(declares(*shard, hash)) {
                    consider(hash, identity, path, 0, file);
                }
                return true;
            });
        }
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

}  // namespace

std::vector<IndexQuery::Located> search(const IndexQuery& index, const SymbolQuery& query,
                                                    std::size_t limit) {
    ScopedTimer timer;
    if(limit == 0 || !query.by_pattern()) {
        return {};
    }
    // A symbol no source places cedes its slot to the next one: the
    // ranking is fetched wider until the cut is filled or exhausted.
    std::vector<IndexQuery::Located> results;
    for(std::size_t fetch = limit * 2;; fetch *= 4) {
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

Completables completables(const IndexQuery& index, llvm::StringRef prefix,
                                                  Fid file,
                                                  std::size_t limit) {
    SymbolQuery query{.pattern = prefix.str(), .external_only = true};
    // Members outnumber the names of namespaces: the ranking is fetched
    // wider than the names wanted.
    auto ranked = ranked_search(index, query, limit * 8);
    llvm::DenseSet<SymbolHash> seen;
    for(auto& hit: ranked.hits) {
        seen.insert(hit.symbol.hash);
    }
    if(auto preamble = index.preamble_index(file)) {
        NameRanker ranker(query);
        preamble->iterate_symbols(
            [&](SymbolHash hash, const SymbolView& identity, llvm::ArrayRef<std::uint32_t>) {
                if(!has_flag(identity.flags, SymbolFlags::Completable) ||
                   identity.scope != SymbolScope::External || !is_searchable_kind(identity.kind) ||
                   !seen.insert(hash).second) {
                    return true;
                }
                auto quality = symbol_quality(identity.name, identity.kind, identity.flags, 0);
                if(auto rank = ranker.rank(identity.name, identity.args, quality)) {
                    ranked.hits.push_back(
                        {.rank = *rank, .symbol = SymbolInfo::from(hash, identity), .anchor = {}});
                }
                return true;
            });
        std::ranges::sort(ranked.hits, [](const Ranked& lhs, const Ranked& rhs) {
            return ranks_after(rhs.rank,
                               rhs.symbol.name,
                               rhs.symbol.args,
                               rhs.symbol.hash,
                               lhs.rank,
                               lhs.symbol.name,
                               lhs.symbol.args,
                               lhs.symbol.hash);
        });
    }

    Completables result{.exhausted = ranked.exhausted};
    for(auto& hit: ranked.hits) {
        if(!has_flag(hit.symbol.flags, SymbolFlags::Completable)) {
            continue;
        }
        if(result.names.size() == limit) {
            result.exhausted = false;
            break;
        }
        Fid declared;
        if(auto identity = index.project().find_symbol(hit.symbol.hash);
           identity && identity->file != no_file) {
            declared = Fid{identity->file};
        }
        result.names.push_back({.symbol = std::move(hit.symbol), .file = declared});
    }
    return result;
}

std::vector<IndexQuery::Located> locate(const IndexQuery& index, const SymbolQuery& query) {
    if(query.handle) {
        Fid anchor;
        for(auto& path: query.paths) {
            if(auto file = index.file_table().find(Spelling::absolute(path))) {
                anchor = *file;
                break;
            }
        }
        if(auto located = index.resolve(*query.handle, anchor)) {
            return {std::move(*located)};
        }
        return {};
    }

    if(query.position) {
        auto& place = *query.position;
        auto path_id = index.file_table().find(Spelling::absolute(place.path));
        if(!path_id) {
            return {};
        }
        // A stale index describes text that no longer exists: resolving the
        // requested place against it would name the wrong symbol.
        auto source = index.serving(*path_id);
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
            auto cursor = index.symbol_at(*path_id, offset);
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
            if(auto info = index.symbol_info(hash, *path_id)) {
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
    for(std::size_t limit = 64;; limit *= 4) {
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
           llvm::any_of(index.targets(hit.symbol.hash, hit.anchor, RelationKind::Primary),
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
