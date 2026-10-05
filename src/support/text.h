#pragma once

#include <cstdint>

#include "llvm/ADT/StringRef.h"

namespace clice {

/// The first byte of the line containing `offset`.
inline std::uint32_t line_begin(llvm::StringRef content, std::uint32_t offset) {
    auto newline = content.rfind('\n', offset);
    return newline == llvm::StringRef::npos ? 0 : static_cast<std::uint32_t>(newline + 1);
}

/// One past the newline ending the line containing `offset`, or the
/// content's end.
inline std::uint32_t line_end(llvm::StringRef content, std::uint32_t offset) {
    auto newline = content.find('\n', offset);
    return newline == llvm::StringRef::npos ? static_cast<std::uint32_t>(content.size())
                                            : static_cast<std::uint32_t>(newline + 1);
}

}  // namespace clice
