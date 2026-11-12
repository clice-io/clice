module clice;

import :index.query;
import :index.query.site;
import :semantic.symbol;

namespace clice::index {

llvm::SmallVector<IndexQuery::Asked> IndexQuery::asked(SymbolHash symbol, Fid anchor) const {
    llvm::SmallVector<Asked> relevant{
        {this, symbol}
    };
    if(other_projects.empty()) {
        return relevant;
    }
    // Another project knows the same entity when it declares it in a file
    // this project declares it in: an unrelated project reusing the name
    // (the same symbol id) declares it elsewhere.
    auto declared = [&](const IndexQuery& from, SymbolHash hash) {
        std::vector<Site> sites;
        for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
            sites.append_range(from.sites(hash, anchor, kind));
        }
        return sites;
    };
    auto own = declared(*this, symbol);
    auto info = symbol_info(symbol, anchor);
    llvm::SmallVector<Fid> files{anchor};
    for(auto& site: own) {
        files.push_back(site.file);
    }
    for(auto& other: other_projects) {
        if(ranges::any_of(declared(other, symbol),
                          [&](const Site& site) { return ranges::contains(files, site.file); })) {
            relevant.push_back({&other, symbol});
            continue;
        }
        // The same name at the same place: a file compiled under other
        // flags can hold another declaration there.
        auto same_name = [&](const Site& site) -> std::optional<SymbolHash> {
            auto cursor = other.symbol_at(site.file, site.range.begin);
            if(!info || !cursor || !covers(site, cursor->site)) {
                return std::nullopt;
            }
            for(auto candidate: cursor->symbols) {
                auto named = other.symbol_info(candidate, site.file);
                if(named && named->name == info->name && named->kind == info->kind) {
                    return candidate;
                }
            }
            return std::nullopt;
        };
        for(auto& site: own) {
            if(auto named = same_name(site)) {
                relevant.push_back({&other, *named});
                break;
            }
        }
    }
    return relevant;
}

const IndexQuery* IndexQuery::owner_of(Fid file) const {
    if(buffer_of(file)) {
        return this;
    }
    for(auto& other: other_projects) {
        if(other.buffer_of(file)) {
            return &other;
        }
    }
    return nullptr;
}

bool IndexQuery::answers_for(const IndexQuery& from, Fid file, llvm::ArrayRef<Asked> asked) const {
    auto* serving = owner_of(file);
    return !serving || serving == &from ||
           ranges::none_of(asked, [&](const Asked& source) { return source.query == serving; });
}

std::vector<Site> IndexQuery::gather(
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
