module;

#include "modules/prelude.h"

module clice;

import :index.peer_queries;
import :index.query;
import :index.site;
import :semantic.symbol;

namespace clice::index {

/// The peers knowing the entity `symbol` names in the own project (see
/// PeerQueries::asked).
static llvm::SmallVector<PeerQueries::Asked> peers_of(const PeerQueries& queries,
                                                      SymbolHash symbol,
                                                      Fid anchor) {
    auto& query = queries.own;
    llvm::SmallVector<PeerQueries::Asked> relevant;
    auto others = queries.peers();
    if(others.empty()) {
        return relevant;
    }
    // A peer knows the same entity when it declares it in a file this
    // project declares it in: an unrelated project reusing the name (the
    // same symbol id) declares it elsewhere.
    auto declared = [&](const IndexQuery& from, SymbolHash hash) {
        std::vector<Site> sites;
        for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
            sites.append_range(from.sites(hash, anchor, kind));
        }
        return sites;
    };
    auto own = declared(query, symbol);
    auto info = query.symbol_info(symbol, anchor);
    llvm::SmallVector<Fid> files{anchor};
    for(auto& site: own) {
        files.push_back(site.file);
    }
    for(auto* peer: others) {
        if(std::ranges::any_of(declared(*peer, symbol), [&](const Site& site) {
               return std::ranges::contains(files, site.file);
           })) {
            relevant.push_back({peer, symbol});
            continue;
        }
        // The same name at the same place: a file compiled under other
        // flags can hold another declaration there.
        auto same_name = [&](const Site& site) -> std::optional<SymbolHash> {
            auto cursor = peer->symbol_at(site.file, site.range.begin);
            if(!info || !cursor || !covers(site, cursor->site)) {
                return std::nullopt;
            }
            for(auto candidate: cursor->symbols) {
                auto named = peer->symbol_info(candidate, site.file);
                if(named && named->name == info->name && named->kind == info->kind) {
                    return candidate;
                }
            }
            return std::nullopt;
        };
        for(auto& site: own) {
            if(auto named = same_name(site)) {
                relevant.push_back({peer, *named});
                break;
            }
        }
    }
    return relevant;
}

llvm::SmallVector<PeerQueries::Asked> PeerQueries::asked(SymbolHash symbol, Fid anchor) const {
    llvm::SmallVector<Asked> all{
        {&own, symbol}
    };
    all.append(peers_of(*this, symbol, anchor));
    return all;
}

std::string PeerQueries::qualified_name(SymbolHash symbol, Fid anchor) const {
    if(auto name = own.qualified_name(symbol, anchor); !name.empty()) {
        return name;
    }
    for(auto* peer: peers()) {
        if(auto name = peer->qualified_name(symbol, anchor); !name.empty()) {
            return name;
        }
    }
    return {};
}

bool PeerQueries::answers_for(const IndexQuery& from, Fid file, llvm::ArrayRef<Asked> asked) const {
    auto* serving = open_in(file);
    return !serving || serving == &from || std::ranges::none_of(asked, [&](const Asked& source) {
        return source.query == serving;
    });
}

std::vector<Site> PeerQueries::gather(
    llvm::ArrayRef<SymbolHash> symbols,
    Fid anchor,
    llvm::function_ref<std::vector<Site>(const IndexQuery&, SymbolHash)> ask) const {
    std::vector<Site> sites;
    for(auto symbol: symbols) {
        auto asked = this->asked(symbol, anchor);
        for(auto& [from, named]: asked) {
            for(auto& site: ask(*from, named)) {
                if(answers_for(*from, site.file, asked)) {
                    sites.push_back(std::move(site));
                }
            }
        }
    }
    dedup_sites(sites);
    return sites;
}

}  // namespace clice::index
