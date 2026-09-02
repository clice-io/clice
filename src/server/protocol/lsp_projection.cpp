#include "server/protocol/lsp_projection.h"

#include <format>
#include <string>
#include <variant>

#include "feature/feature.h"

#include "llvm/ADT/StringRef.h"

namespace clice::to_lsp {

std::optional<protocol::Range> range(const Site& site) {
    return site.coords.to_range(site.range.begin, site.range.end);
}

std::optional<protocol::Location> location(const Site& site) {
    auto mapped = range(site);
    if(!mapped) {
        return std::nullopt;
    }
    return protocol::Location{.uri = feature::to_uri(site.path), .range = *mapped};
}

std::vector<protocol::Location> locations(llvm::ArrayRef<Site> sites) {
    std::vector<protocol::Location> result;
    result.reserve(sites.size());
    for(const auto& site: sites) {
        if(auto mapped = location(site)) {
            result.push_back(std::move(*mapped));
        }
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

std::optional<protocol::SymbolInformation> symbol_information(const index::SymbolRef& symbol,
                                                              const Site& site) {
    auto mapped = location(site);
    if(!mapped) {
        return std::nullopt;
    }
    protocol::SymbolInformation info;
    info.name = symbol.name;
    info.kind = symbol_kind(symbol.kind);
    info.location = std::move(*mapped);
    return info;
}

template <typename Item>
static std::optional<Item> hierarchy_item(const index::SymbolRef& symbol, const Site& site) {
    auto mapped = location(site);
    if(!mapped) {
        return std::nullopt;
    }
    Item item;
    item.name = symbol.name;
    item.kind = symbol_kind(symbol.kind);
    item.uri = std::move(mapped->uri);
    item.range = mapped->range;
    item.selection_range = mapped->range;
    item.data = protocol::LSPAny(std::format("{}", symbol.hash));
    return item;
}

std::optional<protocol::CallHierarchyItem> call_hierarchy_item(const index::SymbolRef& symbol,
                                                               const Site& site) {
    return hierarchy_item<protocol::CallHierarchyItem>(symbol, site);
}

std::optional<protocol::TypeHierarchyItem> type_hierarchy_item(const index::SymbolRef& symbol,
                                                               const Site& site) {
    return hierarchy_item<protocol::TypeHierarchyItem>(symbol, site);
}

std::optional<index::SymbolHash> hierarchy_symbol(const std::optional<protocol::LSPAny>& data) {
    if(!data) {
        return std::nullopt;
    }
    auto* str = std::get_if<std::string>(&static_cast<const protocol::LSPVariant&>(*data));
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
