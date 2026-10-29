module clice:feature.inactive_regions;

import :compile.compilation;
import :support.prelude;

namespace clice::feature {

/// Result of scanning a unit for preprocessor-inactive regions.
struct InactiveScan {
    /// Byte-offset ranges [begin0, end0, begin1, end1, ...] of inactive
    /// branch bodies in the focus file; directive lines excluded.
    /// Sorted by begin and disjoint — nested inactive conditionals merge
    /// into the enclosing region.
    std::vector<std::uint32_t> regions;

    /// Conditional levels still open at the end of the scanned content,
    /// outermost first. Bit 0: the level's current branch is inactive.
    /// Bit 1: an earlier branch of the level was already taken (decides a
    /// later #else). Preamble/PCH builds end mid-#if when the bound cuts
    /// inside a block; the AST compile resumes from this stack.
    std::vector<std::uint8_t> open_stack;
};

/// Scan the focus file's condition directives. `open_stack` seeds the
/// nesting state (from a preceding preamble scan) and `resume_offset` is
/// where the scanned content starts — pending inactive levels from the
/// seed begin there. A scan that ends with open levels closes their
/// pending regions at `end_offset` (the content bound).
InactiveScan inactive_regions(CompilationUnit& unit,
                              llvm::ArrayRef<std::uint8_t> open_stack = {},
                              std::uint32_t resume_offset = 0,
                              std::uint32_t end_offset = UINT32_MAX);

}  // namespace clice::feature
