module;

#include "modules/prelude.h"

module clice:feature.selection_ranges;

import :compile.compilation;
import :syntax.position;
import :syntax.source_text;

namespace clice::feature {

namespace protocol = kota::ipc::protocol;

/// The ranges an expanding selection steps through from each offset of the
/// focus file, innermost first, each strictly containing the one before:
/// the AST nodes around the offset merged with the ranges only the text
/// shows — a literal's text, a comment, the inside of a bracket pair, a
/// statement with its `;`, a directive line. An offset nothing covers gets
/// the empty range at it.
auto selection_ranges(CompilationUnit& unit, llvm::ArrayRef<std::uint32_t> offsets)
    -> std::vector<std::vector<LocalSourceRange>>;

/// The ranges only the text shows, from its lexical scan: what a document
/// without an AST gets.
auto lexical_selection_ranges(SourceText& source, llvm::ArrayRef<std::uint32_t> offsets)
    -> std::vector<std::vector<LocalSourceRange>>;

/// Protocol encoding of one offset's ranges as a chain of parents.
auto selection_range_to_protocol(llvm::ArrayRef<LocalSourceRange> ranges,
                                 const PositionMap& map,
                                 PositionEncoding encoding) -> protocol::SelectionRange;

}  // namespace clice::feature
