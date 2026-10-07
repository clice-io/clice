module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :server.lsp_projection;

namespace clice::to_lsp {

protocol::Range range(const index::Site& site) {
    return {
        .start = {.line = site.begin.line, .character = site.begin.utf16_column},
        .end = {.line = site.end.line,   .character = site.end.utf16_column  },
    };
}

protocol::Location location(const index::Site& site) {
    return {.uri = feature::to_uri(site.path), .range = range(site)};
}

std::vector<protocol::Location> locations(llvm::ArrayRef<index::Site> sites) {
    std::vector<protocol::Location> result;
    result.reserve(sites.size());
    for(const auto& site: sites) {
        result.push_back(location(site));
    }
    return result;
}

std::vector<protocol::Range> ranges(llvm::ArrayRef<index::Site> sites) {
    std::vector<protocol::Range> result;
    result.reserve(sites.size());
    for(const auto& site: sites) {
        result.push_back(range(site));
    }
    return result;
}

protocol::SymbolKind symbol_kind(SymbolKind kind) {
    switch(kind) {
        case SymbolKind::Type: return protocol::SymbolKind::TypeParameter;
        case SymbolKind::Concept: return protocol::SymbolKind::Interface;
        case SymbolKind::Macro: return protocol::SymbolKind::Function;
        default: return feature::to_protocol_symbol_kind(kind);
    }
}

protocol::SymbolInformation symbol_information(const index::SymbolRef& symbol,
                                               const index::Site& site,
                                               llvm::StringRef container) {
    protocol::SymbolInformation info;
    info.name = symbol.display_name();
    info.kind = symbol_kind(symbol.kind);
    if(!container.empty()) {
        info.container_name = container.str();
    }
    info.location = location(site);
    return info;
}

template <typename Item>
static Item hierarchy_item(const index::SymbolRef& symbol,
                           const index::Site& site,
                           const index::Site& extent) {
    Item item;
    item.name = symbol.display_name();
    item.kind = symbol_kind(symbol.kind);
    item.uri = feature::to_uri(site.path);
    item.range = range(extent);
    item.selection_range = range(site);
    item.data = protocol::LSPAny(std::format("{}", symbol.hash));
    return item;
}

protocol::CallHierarchyItem call_hierarchy_item(const index::SymbolRef& symbol,
                                                const index::Site& site,
                                                const index::Site& extent) {
    return hierarchy_item<protocol::CallHierarchyItem>(symbol, site, extent);
}

protocol::TypeHierarchyItem type_hierarchy_item(const index::SymbolRef& symbol,
                                                const index::Site& site,
                                                const index::Site& extent) {
    return hierarchy_item<protocol::TypeHierarchyItem>(symbol, site, extent);
}

std::optional<index::SymbolHash> hierarchy_symbol(const std::optional<protocol::LSPAny>& data) {
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
