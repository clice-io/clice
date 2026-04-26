#pragma once

#include <string>

#include "llvm/ADT/StringRef.h"

namespace clice {

int run_agentic_mode(llvm::StringRef host, int port, llvm::StringRef path);

}  // namespace clice
