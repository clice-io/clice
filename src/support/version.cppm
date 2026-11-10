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

    /// The commit the binary was built from, empty for a non-git build.
    extern const std::string_view commit;
}

/// Where a reader of a log finds the source of `commit`.
constexpr inline std::string_view source_tree = "https://github.com/clice-io/clice/tree/";

/// The line a log starts with after the version's: the source of the
/// build's commit, with local changes when the version says so; empty for a
/// non-git build.
inline std::string built_from() {
    if(commit.empty()) {
        return {};
    }
    return std::format("Built from {}{}{}",
                       source_tree,
                       commit,
                       version.ends_with("-dirty") ? " with local changes" : "");
}

}  // namespace clice
