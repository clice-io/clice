module;

#include "modules/prelude.h"

module clice;

import :server.uri;

namespace clice {

namespace lsp = kota::ipc::lsp;

std::optional<Spelling> uri_to_path(llvm::StringRef uri) {
    auto parsed = lsp::URI::parse(uri.str());
    if(!parsed) {
        return std::nullopt;
    }
    auto path = parsed->file_path();
    if(!path || !path::is_absolute(*path)) {
        return std::nullopt;
    }
    return Spelling::absolute(*path);
}

}  // namespace clice
