#pragma once

#include <string_view>

namespace clice {

/// Version string baked at build time: `git describe` relative to the
/// latest tag when building from a repository (a tag-exact build prints
/// the tag itself; without a reachable tag the base version plus the
/// commit hash), the project's base version for non-git builds
/// (bazel/workspace_status.mjs).
///
/// Defined in a stamped source of its own (BUILD.bazel): a disk or remote
/// cache keys every compile on all headers its target can see, so a header
/// carrying the version would miss the whole build on every commit.
extern const std::string_view version;

/// Target platform this binary was built for, e.g. "x86_64-unknown-linux-gnu".
/// Logged with the version so crash reports identify the exact artifact.
extern const std::string_view target;

}  // namespace clice
