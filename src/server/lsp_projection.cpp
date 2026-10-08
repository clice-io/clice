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

std::vector<protocol::DocumentHighlight>
    document_highlights(llvm::ArrayRef<index::IndexQuery::PlacedHighlight> highlights) {
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
        result.push_back({.range = range(highlight.site), .kind = kind_of(highlight.kind)});
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

protocol::LSPAny inlay_hint_data(llvm::StringRef uri,
                                 llvm::ArrayRef<feature::InlayHintPart> label) {
    kota::codec::dyn::Array parts;
    for(const auto& part: label) {
        if(!part.symbol) {
            parts.push_back(nullptr);
            continue;
        }
        kota::codec::dyn::Object target{
            {"symbol", std::format("{}", part.symbol)}
        };
        if(!part.anchor.empty()) {
            target.insert("anchor", part.anchor);
        }
        parts.push_back(std::move(target));
    }
    return kota::codec::dyn::Object{
        {"uri",   uri.str()       },
        {"parts", std::move(parts)},
    };
}

std::optional<InlayHintData> parse_inlay_hint_data(const std::optional<protocol::LSPAny>& data) {
    const auto* object = data ? data->get_object() : nullptr;
    const auto* uri = object ? object->find("uri") : nullptr;
    const auto* parts = object ? object->find("parts") : nullptr;
    if(!uri || !uri->get_string() || !parts || !parts->get_array()) {
        return std::nullopt;
    }
    InlayHintData result{.uri = std::string(*uri->get_string())};
    for(const auto& part: *parts->get_array()) {
        auto& piece = result.label.emplace_back();
        const auto* target = part.get_object();
        if(!target) {
            continue;
        }
        const auto* symbol = target->find("symbol");
        if(!symbol || !symbol->get_string() ||
           llvm::StringRef(*symbol->get_string()).getAsInteger(10, piece.symbol)) {
            return std::nullopt;
        }
        if(const auto* anchor = target->find("anchor")) {
            auto path = anchor->get_string();
            if(!path || !llvm::sys::path::is_absolute(*path)) {
                return std::nullopt;
            }
            piece.anchor = *path;
        }
    }
    return result;
}

bool is_null(const kota::codec::RawValue& raw) {
    return raw.data == "null";
}

bool is_empty(const kota::codec::RawValue& raw) {
    return raw.data == "[]" || raw.data == "null";
}

}  // namespace clice::to_lsp
