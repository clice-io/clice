module;

#include "support/prelude.h"

module clice:feature.formatting;

import :syntax.token;

namespace clice::feature {

/// The edits formatting `content`, or its `range`, with the clang-format
/// style of `file`: the nearest `.clang-format` above it, else LLVM's. Why
/// that style cannot be read otherwise.
auto document_format(llvm::StringRef file,
                     llvm::StringRef content,
                     std::optional<LocalSourceRange> range = std::nullopt)
    -> std::expected<std::vector<TextReplacement>, std::string>;

/// Reformat the lines `edits` touch with the clang-format style configured
/// for the file, folding the formatting back into replacements of the
/// original text. Without a configured style, or with one that disables
/// formatting, the edits come back unchanged.
auto format_edits(llvm::StringRef file, llvm::StringRef content, std::vector<TextReplacement> edits)
    -> std::vector<TextReplacement>;

/// `edits` cleaned up as clang-tidy cleans up the fixes it applies: on the
/// lines they touch, a comma left after `(`, before `)` or doubled, a
/// constructor's `:` with no initializer left, a namespace left empty.
auto cleanup_edits(llvm::StringRef file,
                   llvm::StringRef content,
                   std::vector<TextReplacement> edits) -> std::vector<TextReplacement>;

/// `text` reformatted as a standalone snippet with the style configured
/// for `file`; unchanged without one.
auto format_snippet(llvm::StringRef file, llvm::StringRef text) -> std::string;

}  // namespace clice::feature
