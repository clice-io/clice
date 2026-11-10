/// What the text around the completion point says is being completed,
/// before any compile: the name under the cursor, a directive's, a header's
/// or a module's, the access operator before it, the designators written.

module clice:feature.code_completion.cursor;

import :support.prelude;
import :syntax.lexer;
import :syntax.token;

namespace clice::feature {

/// The name the cursor is completing, where the text tells: a directive's
/// name, a header name, or the name of an import or a module declaration.
struct CompletionName {
    enum class Kind : std::uint8_t {
        None,
        /// The name of a directive, after its `#`.
        Directive,
        IncludeQuoted,
        IncludeAngled,
        Import,
        /// The name in an implementation unit's `module` declaration.
        ModuleDeclaration,
    };

    Kind kind = Kind::None;
    std::string prefix;

    /// What a chosen candidate replaces: the name under the cursor — of an
    /// include, its last path component — typed part and untyped rest.
    LocalSourceRange replace;

    /// The name ends in its delimiter already: a header name's `>` or `"`,
    /// an import's or a module declaration's semicolon.
    bool closed = false;

    /// On a directive's line past its name, the name: `include` in
    /// `#include <vec`, `ifdef` in `#ifdef FO`. Empty off a directive, and
    /// while the name itself is typed (Kind::Directive).
    std::string directive;

    /// For Kind::Directive: the conditional block open around the line.
    enum class Conditional : std::uint8_t {
        None,
        /// `#elif`, `#else` and `#endif` continue it.
        Open,
        /// It has taken its `#else`: only `#endif` continues it.
        Else,
    };

    Conditional conditional = Conditional::None;
};

/// The name the cursor at `offset` is in: a directive's name, the header
/// name of an #include or a `__has_include`, or the name of an import or a
/// module declaration. Pure text parsing — no compiler state needed.
CompletionName completion_name(llvm::StringRef text,
                               std::uint32_t offset,
                               const clang::LangOptions& dialect);

/// Whether the code before `offset` ends in a member or scope access
/// operator (`.`, `->`, `::`) — outside a directive, the only trigger
/// characters worth a candidate build. A pack ellipsis is not a member
/// access. The cursor's logical line is lexed alone: one inside a block
/// comment reads as code, which costs no more than a build.
bool follows_access_operator(llvm::StringRef text,
                             std::uint32_t offset,
                             const clang::LangOptions& dialect);

/// A designator written in a braced initializer, by the field it starts
/// with.
struct WrittenDesignator {
    llvm::StringRef field;
    /// Whether it designates the field as a whole (`.inner = {}`) rather
    /// than a part of it (`.inner.x = 1`, `.items[0] = 1`).
    bool whole;
};

/// The designators the braced initializer around `offset` has already,
/// where the text before `offset` ends in a `.` starting a designator of
/// its own (`{ .width = 800, .`), in order. Empty anywhere else, after a
/// `.` continuing a designator (`.inner.`) too.
std::vector<WrittenDesignator> written_designators(llvm::StringRef text,
                                                   std::uint32_t offset,
                                                   const clang::LangOptions& dialect);

/// The identifier the completion point sits in.
struct Prefix {
    /// From its start to the cursor: what candidates are matched against.
    LocalSourceRange typed;

    /// Through its end: what a chosen candidate replaces.
    LocalSourceRange whole;

    llvm::StringRef spelling;

    static auto from(llvm::StringRef content, std::uint32_t offset) -> Prefix {
        assert(offset <= content.size());

        auto start = offset;
        while(start > 0 && is_identifier_char(content[start - 1])) {
            start -= 1;
        }

        auto end = offset;
        while(end < content.size() && is_identifier_char(content[end])) {
            end += 1;
        }

        return Prefix{
            .typed = LocalSourceRange(start, offset),
            .whole = LocalSourceRange(start, end),
            .spelling = content.substr(start, offset - start),
        };
    }
};

}  // namespace clice::feature
