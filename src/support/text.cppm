module clice:support.text;

import :support.prelude;

namespace clice {

/// The first byte of the line containing `offset`.
inline std::uint32_t line_begin(llvm::StringRef content, std::uint32_t offset) {
    auto newline = content.rfind('\n', offset);
    return newline == llvm::StringRef::npos ? 0 : static_cast<std::uint32_t>(newline + 1);
}

/// The first byte of the logical line containing `offset`: its line, or
/// the first of the lines a backslash continues onto it.
inline std::uint32_t logical_line_begin(llvm::StringRef content, std::uint32_t offset) {
    auto begin = line_begin(content, offset);
    while(begin > 0 && content.take_front(begin - 1).rtrim(" \t\r").ends_with("\\")) {
        begin = line_begin(content, begin - 1);
    }
    return begin;
}

/// Whether only whitespace precedes `offset` on its line.
inline bool begins_line(llvm::StringRef content, std::uint32_t offset) {
    return content.slice(line_begin(content, offset), offset).trim().empty();
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

/// Source text without its line splices: `SPLIT_\<newline>LIMIT` spells
/// `SPLIT_LIMIT`.
inline std::string unspliced(llvm::StringRef text) {
    std::string result;
    for(auto splice = text.find('\\'); splice != llvm::StringRef::npos; splice = text.find('\\')) {
        auto length = splice_length(text.drop_front(splice));
        result += text.take_front(length != 0 ? splice : splice + 1);
        text = text.drop_front(splice + std::max<std::size_t>(length, 1));
    }
    result += text;
    return result;
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

}  // namespace clice
