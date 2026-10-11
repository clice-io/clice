module;

#include "modules/prelude.h"

module clice:semantic.designators;

namespace clice::designators {

/// The designator each initializer of a syntactic init list leaves
/// unwritten, keyed by the initializer's begin location: `.x` for the `1`
/// in `Point{1}`, `[2]` for an array element, `.inner.x` through elided
/// braces. Initializers with no nameable designator are left out.
auto unwritten(const clang::InitListExpr* syntactic)
    -> llvm::DenseMap<clang::SourceLocation, std::string>;

}  // namespace clice::designators
