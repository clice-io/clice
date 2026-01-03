#pragma once

#include "llvm/ADT/DenseMap.h"
#include "clang/Basic/SourceLocation.h"

namespace clice {

class CompilationUnitRef;

}

namespace clice::index {

template <typename T>
using Shared = llvm::DenseMap<clang::FileID, T>;

}
