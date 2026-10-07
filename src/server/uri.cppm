module;

#include "modules/prelude.h"

module clice:server.uri;

import :vfs.path;

namespace clice {

/// The file a `file:` URI names; nullopt for any other URI.
std::optional<Spelling> uri_to_path(llvm::StringRef uri);

}  // namespace clice
