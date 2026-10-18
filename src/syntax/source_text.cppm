module;

#include "modules/prelude.h"

module clice:syntax.source_text;

import :syntax.lexical_scan;
import :syntax.position;

namespace clice {

/// One text and what its readers derive from it, each made on first use
/// and kept: its line tables, its comments, its lexical scan. Borrows the
/// text, which a NUL must follow as the lexer reads one, and the dialect
/// it is read in.
class SourceText {
public:
    SourceText(llvm::StringRef text, const clang::LangOptions& dialect) :
        content(text), dialect(dialect) {}

    SourceText(const SourceText&) = delete;
    SourceText& operator=(const SourceText&) = delete;

    llvm::StringRef text() const {
        return content;
    }

    auto positions() -> PositionMap;

    auto lexical() -> const LexicalInfo&;

    /// The lexical scan's comments once it ran, else those of a scan for
    /// the comments alone.
    auto comments() -> llvm::ArrayRef<LexicalInfo::Comment>;

private:
    llvm::StringRef content;

    const clang::LangOptions& dialect;

    struct LineTables {
        std::vector<std::uint32_t> starts;
        std::vector<std::uint64_t> non_ascii;
    };

    std::optional<LineTables> lines;

    std::optional<LexicalInfo> scan;

    std::optional<std::vector<LexicalInfo::Comment>> comment_scan;
};

}  // namespace clice
