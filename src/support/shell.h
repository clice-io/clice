#pragma once

#include <string>
#include <vector>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/StringSaver.h"

namespace clice {

/// Split a command line into arguments with the host's shell rules — the
/// rules a compile_commands.json `command` field is read with.
inline std::vector<std::string> tokenize_command(llvm::StringRef command) {
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver saver(allocator);
    llvm::SmallVector<const char*, 32> arguments;
#ifdef _WIN32
    llvm::cl::TokenizeWindowsCommandLineFull(command, saver, arguments);
#else
    llvm::cl::TokenizeGNUCommandLine(command, saver, arguments);
#endif
    return {arguments.begin(), arguments.end()};
}

}  // namespace clice
