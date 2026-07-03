#pragma once

#include <cstdint>
#include <vector>

#include "compile/compilation_unit.h"

namespace clice::feature {

/// Byte-offset ranges of preprocessor-inactive regions in the interested
/// file: the bodies of #if/#elif/#else branches whose condition evaluated
/// to false (or that were skipped because an earlier branch was taken).
/// Directive lines themselves are excluded. Flat pairs [begin0, end0,
/// begin1, end1, ...] to keep the worker protocol simple.
std::vector<std::uint32_t> inactive_regions(CompilationUnitRef unit);

}  // namespace clice::feature
