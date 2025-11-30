#pragma once

#include "llvm/ADT/SmallVector.h"
#include "clang/AST/Decl.h"

namespace clice::index {

bool generateUSRForDecl(const clang::Decl* D, llvm::SmallVectorImpl<char>& buffer);

bool generateUSRForMacro(llvm::StringRef name,
                         clang::SourceLocation location,
                         const clang::SourceManager& SM,
                         llvm::SmallVectorImpl<char>& buffer);

}  // namespace clice::index
