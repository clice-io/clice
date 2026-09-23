#pragma once

#include <string>

namespace clice {

/// Convert a file:// URI to a local file path.
std::string uri_to_path(const std::string& uri);

}  // namespace clice
