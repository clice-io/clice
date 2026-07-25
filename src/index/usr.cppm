export module clice.index:usr;

import llvm;
import clang;

export namespace clice::index {

bool generateUSRForDecl(const clang::Decl* D, llvm::SmallVectorImpl<char>& buffer);

bool generateUSRForMacro(llvm::StringRef name,
                         clang::SourceLocation location,
                         const clang::SourceManager& SM,
                         llvm::SmallVectorImpl<char>& buffer);

}  // namespace clice::index
