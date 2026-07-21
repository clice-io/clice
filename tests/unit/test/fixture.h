#pragma once

#include "llvm/ADT/StringRef.h"

namespace clice::testing {

/// Fixture files under tests/data/<feature>/ may begin with a doc header
/// consumed by the feature-doc generator (tests/tools/feature_docs.py):
///
///     /// # Block folding
///     ///
///     /// - status: supported
///     /// - issues: clangd#1455
///     ///
///     /// Optional markdown description after a bare `///` separator.
///
/// A file has a header iff its first line (trimmed) starts with `/// #`.
/// Returns the value of the metadata list entry `- key: value` inside the
/// leading run of `///` lines, or an empty StringRef when the file has no
/// header or the key is absent. This is a deliberately trivial scan; the
/// Python parser is the authority on the full grammar.
inline llvm::StringRef fixture_frontmatter(llvm::StringRef content, llvm::StringRef key) {
    // Mirror parse_fixture's title grammar exactly: strip `///` plus one
    // space, then require `# ` — so `///#x`, `/// ## h2` or extra-indented
    // variants are supplementary content on both sides.
    llvm::StringRef first = content.split('\n').first.trim();
    if(!first.consume_front("///")) {
        return {};
    }
    first.consume_front(" ");
    if(!first.starts_with("# ")) {
        return {};
    }
    while(!content.empty()) {
        auto parts = content.split('\n');
        content = parts.second;
        auto line = parts.first.trim();
        if(!line.starts_with("///")) {
            break;
        }
        line = line.drop_front(3).trim();
        if(!line.consume_front("- ") || !line.contains(':')) {
            continue;
        }
        auto kv = line.split(':');
        if(kv.first.trim() == key) {
            return kv.second.trim();
        }
    }
    return {};
}

}  // namespace clice::testing
