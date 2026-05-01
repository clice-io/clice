#pragma once

#include <string>

#include "llvm/ADT/StringRef.h"

namespace clice {

int run_agentic_mode(llvm::StringRef host, int port, llvm::StringRef path);

int run_relay_mode(llvm::StringRef socket_path);

}  // namespace clice
