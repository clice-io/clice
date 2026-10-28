module;

#include "support/prelude.h"

module clice;

import :feature.protocol;
import :index.site;
import :index.symbol;
import :syntax.position;
import :syntax.token;

namespace clice::feature {

protocol::Range to_range(const index::Site& site, PositionEncoding encoding) {
    return {.start = site.begin.position(encoding), .end = site.end.position(encoding)};
}

protocol::Location to_location(const index::Site& site, PositionEncoding encoding) {
    return {.uri = to_uri(site.path), .range = to_range(site, encoding)};
}

std::vector<protocol::Location> to_locations(llvm::ArrayRef<index::Site> sites,
                                             PositionEncoding encoding) {
    std::vector<protocol::Location> result;
    result.reserve(sites.size());
    for(const auto& site: sites) {
        result.push_back(to_location(site, encoding));
    }
    return result;
}

std::vector<protocol::Range> to_ranges(llvm::ArrayRef<index::Site> sites,
                                       PositionEncoding encoding) {
    std::vector<protocol::Range> result;
    result.reserve(sites.size());
    for(const auto& site: sites) {
        result.push_back(to_range(site, encoding));
    }
    return result;
}

std::optional<std::vector<protocol::TextEdit>>
    to_text_edits(llvm::ArrayRef<TextReplacement> replacements,
                  const PositionMap& map,
                  PositionEncoding encoding) {
    std::vector<protocol::TextEdit> edits;
    for(const auto& replacement: replacements) {
        auto converted = map.range(replacement.range, encoding);
        if(!converted) {
            return std::nullopt;
        }
        edits.push_back({.range = *converted, .new_text = replacement.text});
    }
    return edits;
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

}  // namespace clice::feature
