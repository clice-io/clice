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

/// The length of the line splice `text` starts with: a backslash, the
/// blanks clang tolerates after it, and a line break; zero for none.
inline std::size_t splice_length(llvm::StringRef text) {
    if(!text.starts_with('\\')) {
        return 0;
    }
    auto rest = text.drop_front().ltrim(" \t\v\f");
    auto skipped = text.size() - rest.size();
    if(rest.starts_with("\r\n")) {
        return skipped + 2;
    }
    return rest.starts_with('\n') || rest.starts_with('\r') ? skipped + 1 : 0;
}

/// A size in bytes as the commands print it: B, KB or MB.
inline std::string format_bytes(std::uint64_t bytes) {
    if(bytes >= 1024 * 1024) {
        return std::format("{:.1f} MB", bytes / (1024.0 * 1024.0));
    }
    if(bytes >= 1024) {
        return std::format("{:.1f} KB", bytes / 1024.0);
    }
    return std::format("{} B", bytes);
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
