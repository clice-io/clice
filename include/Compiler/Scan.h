#pragma once

#include <string>
#include <vector>

#include "AST/SourceCode.h"

#include "llvm/ADT/StringRef.h"

namespace clice {

struct Inclusion {
    /// Whether this file is braced angles.
    bool angled;

    /// The line of this inclusion(zero based).
    /// std::uint32_t line;

    /// The included file.
    llvm::StringRef file;
};

struct ScanResult {
    /// The module file of this file(may be empty).
    std::vector<Token> module_name;

    /// The includes of file.
    std::vector<Inclusion> includes;
};

/// Scan the file and return necessary info.
ScanResult scan(llvm::StringRef content);

}  // namespace clice
