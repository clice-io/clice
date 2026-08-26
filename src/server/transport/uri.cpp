#include "server/transport/uri.h"

#include "kota/ipc/lsp/uri.h"

namespace clice {

namespace lsp = kota::ipc::lsp;

std::string uri_to_path(const std::string& uri) {
    auto parsed = lsp::URI::parse(uri);
    if(parsed.has_value()) {
        auto path = parsed->file_path();
        if(path.has_value()) {
            return std::move(*path);
        }
    }
    return uri;
}

}  // namespace clice
