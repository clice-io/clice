#include "Compiler/Toolchain.h"

namespace clice::toolchain {

Toolchain query_toolchain(llvm::ArrayRef<const char*> arguments) {
    llvm::StringRef driver = arguments[0];

    /// judge tool chain kind ...

    return {};
}

}  // namespace clice::toolchain
