module clice:support.version;

import :support.prelude;

namespace clice {

/// Defined in a stamped source of its own (bazel/BUILD.bazel), outside the
/// module: a disk or remote cache keys every compile on all it can see, so
/// a partition carrying the version would miss the whole build on every
/// commit. The global module owns the names, which that source defines.
extern "C++" {

    /// Version string baked at build time: `git describe` relative to the
    /// latest tag when building from a repository (a tag-exact build prints
    /// the tag itself; without a reachable tag the base version plus the
    /// commit hash), the project's base version for non-git builds
    /// (bazel/workspace_status.mjs).
    extern const std::string_view version;

    /// Target platform this binary was built for, e.g. "x86_64-unknown-linux-gnu".
    /// Logged with the version so crash reports identify the exact build.
    extern const std::string_view target;
}

}  // namespace clice
