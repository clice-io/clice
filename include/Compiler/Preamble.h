#pragma once

#include <expected>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"

namespace clice {

class CompilationUnit;

struct CompilationParams;

struct PCHInfo {
    /// The path of the output PCH file.
    std::string path;

    /// The building time of this PCH.
    std::int64_t mtime;

    /// The content used to build this PCH.
    std::string preamble;

    /// All files involved in building this PCH.
    std::vector<std::string> deps;

    /// The command arguments used to build this PCH.
    std::vector<const char*> arguments;
};

/// Compute the preamble bound of given content. We just
/// run lex until we find first not directive.
std::uint32_t compute_preamble_bound(llvm::StringRef content);

/// Same as above, but return a group of bounds for chained PCH
/// building.
std::vector<uint32_t> compute_preamble_bounds(llvm::StringRef content);

}  // namespace clice
