module clice:syntax.source_text;

import :support.prelude;
import :syntax.lexer;
import :syntax.lexical_scan;
import :syntax.position;
import :syntax.token;
import :vfs.file_system;

namespace clice {

/// A file's bytes and what its readers derive from them, each made on
/// first use and kept: their hash, and of their text — the bytes
/// without a byte order mark (vfs::without_bom), which every reader in
/// clice reads — its line tables, its comments, its lexical scan. Borrows
/// the bytes, which a NUL must follow as the lexer reads one, or owns them;
/// borrows the dialect it is read in and that dialect's keywords
/// (lexed_kind).
class SourceText {
public:
    /// Borrows `bytes`, read in `dialect` with `keywords`.
    SourceText(llvm::StringRef bytes,
               const clang::LangOptions& dialect,
               const clang::IdentifierTable& keywords) :
        raw(bytes), content(vfs::without_bom(bytes)), options(&dialect), table(&keywords) {}

    /// Borrows `bytes`, read in `dialect` with its keywords().
    SourceText(llvm::StringRef bytes, const clang::LangOptions& dialect) :
        SourceText(bytes, dialect, clice::keywords(dialect)) {}

    /// Owns `bytes`, read in no dialect until set_dialect.
    explicit SourceText(std::string bytes) :
        owned(std::move(bytes)), raw(owned), content(vfs::without_bom(owned)) {}

    SourceText(const SourceText&) = delete;
    SourceText& operator=(const SourceText&) = delete;

    llvm::StringRef text() const {
        return content;
    }

    /// The bytes, as a compile reads them.
    llvm::StringRef bytes() const {
        return raw;
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

    /// The bytes' hash (vfs::content_hash): what "a buffer holds these
    /// bytes" is compared by against disk observations and index shards.
    auto hash() -> std::uint64_t;

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

    llvm::StringRef raw;

    llvm::StringRef content;

    const clang::LangOptions* options = nullptr;

    const clang::IdentifierTable* table = nullptr;

    std::optional<std::uint64_t> bytes_hash;

    struct LineTables {
        std::vector<std::uint32_t> starts;
        std::vector<std::uint64_t> non_ascii;
    };

    std::optional<LineTables> lines;

    std::optional<LexicalInfo> scan;

    std::optional<std::vector<LexicalInfo::Comment>> comment_scan;
};

}  // namespace clice
