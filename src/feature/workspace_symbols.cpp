module;

#include "modules/prelude.h"

module clice;

import :feature.document_symbols;
import :feature.protocol;
import :feature.workspace_symbols;
import :index.site;
import :index.symbol;

namespace clice::feature {

protocol::SymbolInformation symbol_information(const index::SymbolInfo& symbol,
                                               const index::Site& site,
                                               llvm::StringRef container) {
    protocol::SymbolInformation info;
    info.name = symbol.display_name() + symbol.signature;
    info.kind = to_protocol_symbol_kind(symbol.kind);
    if(!container.empty()) {
        info.container_name = container.str();
    }
    info.location = to_location(site);
    return info;
}

}  // namespace clice::feature
