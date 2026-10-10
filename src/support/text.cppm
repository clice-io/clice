module;

#include "modules/prelude.h"

module clice:support.text;

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

/// The items of a comma-separated list, each trimmed.
inline std::vector<std::string> comma_list(llvm::StringRef text) {
    llvm::SmallVector<llvm::StringRef> parts;
    text.split(parts, ',', -1, false);
    std::vector<std::string> items;
    for(auto part: parts) {
        items.push_back(part.trim().str());
    }
    return items;
}

}  // namespace clice
