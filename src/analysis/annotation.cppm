module;

#include "modules/prelude.h"

module clice:analysis.annotation;

namespace clice::analysis {

/// A named weight per file, consumed the way PGO consumes a profile: an
/// analysis scales its counts by the annotations present and falls back
/// to plain counts for the ones that are not, so no result depends on
/// having any.
struct Annotation {
    std::string name;
    std::string unit;

    /// Workspace-relative path -> value.
    llvm::StringMap<double> values;
};

/// Commits that touched the file: how often an edit to it happens.
constexpr inline llvm::StringLiteral churn_annotation = "churn";

/// Seconds a translation unit takes to compile: what rebuilding it costs.
constexpr inline llvm::StringLiteral compile_time_annotation = "compile_time";

struct Annotations {
    std::vector<Annotation> list;

    const Annotation* find(llvm::StringRef name) const;

    /// The value `name` gives `path`; `fallback` when the annotation is
    /// absent or says nothing about the file.
    double value(llvm::StringRef name, llvm::StringRef path, double fallback) const;
};

/// An annotation file: `{"name": ..., "unit": ..., "values": {path: number}}`,
/// paths relative to the workspace.
std::expected<Annotation, std::string> read_annotation(llvm::StringRef path);

/// The churn annotation from the workspace's git history: per file, the
/// commits since `since` (any `git log --since` value) that touched it.
std::expected<Annotation, std::string> git_churn(llvm::StringRef workspace, llvm::StringRef since);

}  // namespace clice::analysis
