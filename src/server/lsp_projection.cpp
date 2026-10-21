module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :index.navigation;
import :server.lsp_projection;
import :syntax.position;

namespace clice::to_lsp {

protocol::Range range(const index::Site& site, PositionEncoding encoding) {
    return {.start = site.begin.in(encoding), .end = site.end.in(encoding)};
}

protocol::Location location(const index::Site& site, PositionEncoding encoding) {
    return {.uri = feature::to_uri(site.path), .range = range(site, encoding)};
}

std::vector<protocol::Location> locations(llvm::ArrayRef<index::Site> sites,
                                          PositionEncoding encoding) {
    std::vector<protocol::Location> result;
    result.reserve(sites.size());
    for(const auto& site: sites) {
        result.push_back(location(site, encoding));
    }
    return result;
}

std::vector<protocol::Range> ranges(llvm::ArrayRef<index::Site> sites, PositionEncoding encoding) {
    std::vector<protocol::Range> result;
    result.reserve(sites.size());
    for(const auto& site: sites) {
        result.push_back(range(site, encoding));
    }
    return result;
}

std::vector<protocol::DocumentHighlight>
    document_highlights(llvm::ArrayRef<index::PlacedHighlight> highlights,
                        PositionEncoding encoding) {
    auto kind_of = [](index::HighlightKind kind) {
        switch(kind) {
            case index::HighlightKind::Text: return protocol::DocumentHighlightKind::Text;
            case index::HighlightKind::Read: return protocol::DocumentHighlightKind::Read;
            case index::HighlightKind::Write: return protocol::DocumentHighlightKind::Write;
        }
        std::unreachable();
    };
    std::vector<protocol::DocumentHighlight> result;
    result.reserve(highlights.size());
    for(const auto& highlight: highlights) {
        result.push_back(
            {.range = range(highlight.site, encoding), .kind = kind_of(highlight.kind)});
    }
    return result;
}

protocol::SymbolInformation symbol_information(const index::SymbolInfo& symbol,
                                               const index::Site& site,
                                               llvm::StringRef container,
                                               PositionEncoding encoding) {
    protocol::SymbolInformation info;
    info.name = symbol.display_name() + symbol.signature;
    info.kind = feature::to_protocol_symbol_kind(symbol.kind);
    if(!container.empty()) {
        info.container_name = container.str();
    }
    info.location = location(site, encoding);
    return info;
}

template <typename Item>
static Item hierarchy_item(const index::SymbolInfo& symbol,
                           const index::Site& site,
                           const index::Site& extent,
                           llvm::StringRef qualified,
                           PositionEncoding encoding) {
    Item item;
    item.name = symbol.display_name();
    if(auto detail = qualified.str() + symbol.signature; detail != item.name) {
        item.detail = std::move(detail);
    }
    item.kind = feature::to_protocol_symbol_kind(symbol.kind);
    item.uri = feature::to_uri(site.path);
    item.range = range(extent, encoding);
    item.selection_range = range(site, encoding);
    item.data = protocol::LSPAny(std::format("{}", symbol.hash));
    return item;
}

protocol::CallHierarchyItem call_hierarchy_item(const index::SymbolInfo& symbol,
                                                const index::Site& site,
                                                const index::Site& extent,
                                                llvm::StringRef qualified,
                                                PositionEncoding encoding) {
    return hierarchy_item<protocol::CallHierarchyItem>(symbol, site, extent, qualified, encoding);
}

protocol::TypeHierarchyItem type_hierarchy_item(const index::SymbolInfo& symbol,
                                                const index::Site& site,
                                                const index::Site& extent,
                                                llvm::StringRef qualified,
                                                PositionEncoding encoding) {
    return hierarchy_item<protocol::TypeHierarchyItem>(symbol, site, extent, qualified, encoding);
}

std::optional<index::SymbolHash> data_symbol(const std::optional<protocol::LSPAny>& data) {
    if(!data) {
        return std::nullopt;
    }
    auto str = data->get_string();
    if(!str) {
        return std::nullopt;
    }
    index::SymbolHash hash = 0;
    if(llvm::StringRef(*str).getAsInteger(10, hash)) {
        return std::nullopt;
    }
    return hash;
}

bool is_null(const kota::codec::RawValue& raw) {
    return raw.data == "null";
}

bool is_empty(const kota::codec::RawValue& raw) {
    return raw.data == "[]" || raw.data == "null";
}

}  // namespace clice::to_lsp
