module clice;

import :feature.document_symbols;
import :feature.protocol;
import :feature.workspace_symbols;
import :index.site;
import :index.peer_queries;
import :index.search;
import :index.symbol;
import :index.symbol_query;
import :vfs.path;

namespace clice::feature {

protocol::SymbolInformation symbol_information(const index::SymbolInfo& symbol,
                                               const index::Site& site,
                                               llvm::StringRef container,
                                               PositionEncoding encoding) {
    protocol::SymbolInformation info;
    info.name = symbol.display_name() + symbol.signature;
    info.kind = to_protocol(symbol.kind);
    if(!container.empty()) {
        info.container_name = container.str();
    }
    info.location = to_location(site, encoding);
    return info;
}

std::vector<WorkspaceSymbol> workspace_symbols(const index::PeerQueries& queries,
                                               llvm::StringRef text,
                                               const Spelling& root,
                                               PositionEncoding encoding) {
    auto& query = queries.own;
    std::vector<WorkspaceSymbol> results;
    auto parsed = index::SymbolQuery::parse(text);
    if(!parsed) {
        return results;
    }
    parsed->resolve_paths(root);
    // Some clients (VS Code) filter the replies against the query text
    // again, where a bare name would fail a qualified query: those
    // replies carry the qualified name.
    bool qualified = parsed->absolute || !parsed->scope.empty();
    for(auto& located: index::search(query, *parsed, workspace_symbol_limit)) {
        // Every project answers workspace/symbol (MasterServer): an open
        // file only from the project serving its buffer.
        if(auto* serving = queries.open_in(located.site.file); serving && serving != &query) {
            continue;
        }
        auto container = query.container_name(located.symbol.hash, located.site.file);
        auto info = symbol_information(located.symbol, located.site, container, encoding);
        if(qualified && !container.empty()) {
            info.name = container + "::" + info.name;
        }
        auto source = query.serving(located.site.file);
        results.push_back({
            .info = std::move(info),
            .file = query.project().portable(query.file_table().resolve(located.site.file)),
            .content = source ? source->shard->content_hash() : 0,
        });
    }
    return results;
}

std::vector<protocol::SymbolInformation>
    merge_workspace_symbols(llvm::MutableArrayRef<std::vector<WorkspaceSymbol>> ranked) {
    constexpr std::size_t limit = workspace_symbol_limit;
    std::vector<protocol::SymbolInformation> merged;
    // A file several projects see, a header of one an application includes,
    // lists its symbols once too.
    using Place = std::tuple<std::string, std::uint32_t, std::uint32_t, std::string, std::string>;
    std::set<Place> places;
    std::set<std::pair<Place, std::uint64_t>> copies;
    for(std::size_t rank = 0; merged.size() < limit; rank += 1) {
        bool any = false;
        for(auto& list: ranked) {
            if(rank >= list.size() || merged.size() == limit) {
                continue;
            }
            any = true;
            auto& [info, file, content] = list[rank];
            auto& start = info.location.range.start;
            auto container = info.container_name.value_or("");
            bool new_place =
                places.emplace(info.location.uri, start.line, start.character, info.name, container)
                    .second;
            bool new_copy =
                copies
                    .emplace(
                        Place(std::move(file), start.line, start.character, info.name, container),
                        content)
                    .second;
            if(new_place && new_copy) {
                merged.push_back(std::move(info));
            }
        }
        if(!any) {
            break;
        }
    }
    return merged;
}

}  // namespace clice::feature
