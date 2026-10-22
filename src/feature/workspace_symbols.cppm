module;

#include "modules/prelude.h"

module clice:feature.workspace_symbols;

import :index.site;
import :index.symbol;

namespace clice::feature {

namespace protocol = kota::ipc::protocol;

/// `container` is the qualified name of the symbol's parent, empty at the
/// translation unit. A function is named with its signature, which tells
/// its overloads apart.
protocol::SymbolInformation symbol_information(const index::SymbolInfo& symbol,
                                               const index::Site& site,
                                               llvm::StringRef container);

}  // namespace clice::feature
