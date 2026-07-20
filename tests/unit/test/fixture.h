#pragma once

#include "llvm/ADT/StringRef.h"

namespace clice::testing {

/// Fixture files under tests/data/<feature>/ may begin with a frontmatter
/// key block consumed by the feature-doc generator (tests/tools/feature_docs.py):
///
///     /// section: Fold Kinds
///     /// title: Block folding
///     /// status: supported
///     ///
///     /// Optional markdown description after a bare `///` separator.
///
/// Returns the value of `key` inside the leading key block, or an empty
/// StringRef when the file has no frontmatter or the key is absent. The
/// block ends at the first bare `///`, non-comment or non-`key: value`
/// line, and only line-leading keys match — there is no grammar here that
/// could drift from the Python parser.
inline llvm::StringRef fixture_frontmatter(llvm::StringRef content, llvm::StringRef key) {
    while(!content.empty()) {
        auto parts = content.split('\n');
        content = parts.second;
        auto line = parts.first.trim();
        if(!line.starts_with("///")) {
            break;
        }
        line = line.drop_front(3).trim();
        if(line.empty() || !line.contains(':')) {
            break;
        }
        auto kv = line.split(':');
        if(kv.first.trim() == key) {
            return kv.second.trim();
        }
    }
    return {};
}

}  // namespace clice::testing
