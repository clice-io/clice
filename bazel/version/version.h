#pragma once

#include <string_view>

namespace clice {

/// The `git describe` version (cmake/version.h.in has the rules) and the
/// target triple of this binary. Defined in the stamped version.cpp alone:
/// a remote cache keys every compile on all headers of its dependencies, so
/// a stamped header would miss the whole build on every commit.
extern const std::string_view version;
extern const std::string_view target;

}  // namespace clice
