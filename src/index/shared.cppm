export module clice.index:shared;

import llvm;
import clang;

export namespace clice::index {

template <typename T>
using Shared = llvm::DenseMap<clang::FileID, T>;

}
