#pragma once

#include <cstdint>
#include <string>

namespace clice {

struct HeaderContext {
    /// Absolute file system path of the translation unit
    /// that includes this header.
    std::string tu;

    /// The canonical form of the compilation command used to
    /// compile the translation unit.
    std::string command;

    /// Zero-based ordinal of the include directive in the
    /// translation unit that introduces this header.
    std::uint32_t ordinal;
};

struct TUContext {
    /// The canonical form of the compilation command for
    /// this translation unit.
    std::string command;
};

}  // namespace clice
