#pragma once

#include <optional>

#include "vfs/path.h"

namespace clice {

/// The file a `file:` URI names; nullopt for any other URI.
std::optional<Spelling> uri_to_path(llvm::StringRef uri);

}  // namespace clice
