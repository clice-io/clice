#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice::toolchain {

enum class Kind {};

struct Toolchain {};

/// Query toolchain info according to the original compilation arguments.
Toolchain query_toolchain(llvm::ArrayRef<const char*> arguments);

}  // namespace clice::toolchain
