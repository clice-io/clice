module;

#include "modules/prelude.h"

module clice:syntax.source_text;

import :syntax.lexer;
import :syntax.lexical_scan;
import :syntax.position;
import :syntax.token;

namespace clice {

/// One text and what its readers derive from it, each made on first use
/// and kept: its line tables, its comments, its lexical scan. Borrows the
/// text, which a NUL must follow as the lexer reads one, or owns it; borrows
/// the dialect it is read in and that dialect's keywords (lexed_kind).
class SourceText {
public:
    /// Borrows `text`, read in `dialect` with `keywords`.
    SourceText(llvm::StringRef text,
               const clang::LangOptions& dialect,
               const clang::IdentifierTable& keywords) :
        content(text), options(&dialect), table(&keywords) {}

    /// Borrows `text`, read in `dialect` with its keywords().
    SourceText(llvm::StringRef text, const clang::LangOptions& dialect) :
        SourceText(text, dialect, clice::keywords(dialect)) {}

    /// Owns `text`, read in no dialect until set_dialect.
    explicit SourceText(std::string text) : owned(std::move(text)), content(owned) {}

    SourceText(const SourceText&) = delete;
    SourceText& operator=(const SourceText&) = delete;

    llvm::StringRef text() const {
        return content;
    }

    /// Read the text in `dialect`, with its keywords(), from now on: the
    /// scan and the comments read in another go, and every reference to
    /// them with them. An open file's text keeps its version while a
    /// command reload or a context switch changes its dialect.
    void set_dialect(const clang::LangOptions& dialect);

    const clang::LangOptions& dialect() const {
        assert(options && "the text is read in no dialect yet");
        return *options;
    }

    auto positions() -> PositionMap;

    auto lexical() -> const LexicalInfo&;

    /// The lexical scan's comments once it ran, else those of a scan for
    /// the comments alone.
    auto comments() -> llvm::ArrayRef<LexicalInfo::Comment>;

    /// What `token`, one of this text's, lexes as: a keyword's kind for a
    /// raw identifier spelling one (lexed_kind).
    TokenKind kind(const Token& token) const {
        assert(table && "the text is read in no dialect yet");
        return lexed_kind(token, content, *table);
    }

private:
    std::string owned;

    llvm::StringRef content;

    const clang::LangOptions* options = nullptr;

    const clang::IdentifierTable* table = nullptr;

    struct LineTables {
        std::vector<std::uint32_t> starts;
        std::vector<std::uint64_t> non_ascii;
    };

    std::optional<LineTables> lines;

    std::optional<LexicalInfo> scan;

    std::optional<std::vector<LexicalInfo::Comment>> comment_scan;
};

}  // namespace clice
