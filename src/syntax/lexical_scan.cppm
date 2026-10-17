module;

#include "modules/prelude.h"

module clice:syntax.lexical_scan;

import :syntax.token;

namespace clice {

/// Everything one lexical pass over a file records: its tokens and
/// comments, and what of them neither the AST nor the preprocessor
/// callbacks report — the three module declaration forms (clang reports
/// imports through PPCallbacks, but nothing covers the declarations
/// themselves), every directive line with the conditional blocks around
/// it (the callbacks skip the branches nested in a skipped block, a
/// `#else` behind a taken `#elif`, and everything a preamble PCH
/// consumed), the include guard and the extents of raw string literals
/// (the AST keeps only where a literal's tokens start).
struct LexicalInfo {
    constexpr static std::uint32_t none = ~0u;

    /// A comment, as scan_comments finds it.
    struct Comment {
        enum class Kind : std::uint8_t {
            /// A `//` comment.
            Line,
            /// A `/* */` comment.
            Block,
        };

        Kind kind;
        LocalSourceRange range;
    };

    struct ModuleDeclaration {
        enum class Kind : std::uint8_t {
            /// `module;` introducing the global module fragment.
            GlobalFragment,
            /// `[export] module name[:partition];`
            Declaration,
            /// `module :private;` introducing the private module fragment.
            PrivateFragment,
        };

        Kind kind;

        /// The `export` keyword; valid only when a Declaration has one.
        LocalSourceRange export_keyword;

        /// The `module` keyword.
        LocalSourceRange keyword;

        /// The identifiers of the dotted module name, in source order.
        /// Empty for the fragment introducers.
        llvm::SmallVector<LocalSourceRange, 4> name_parts;

        /// The partition colon; invalid when there is no partition. Also
        /// set for the private fragment (`:private`).
        LocalSourceRange colon;

        /// The identifiers of the dotted partition name; the `private`
        /// keyword for the private fragment.
        llvm::SmallVector<LocalSourceRange, 2> partition_parts;

        /// The written name of a Declaration, partition included.
        LocalSourceRange name_range() const {
            auto& last = partition_parts.empty() ? name_parts.back() : partition_parts.back();
            return {name_parts.front().begin, last.end};
        }
    };

    /// A directive line, whether or not the preprocessor entered it.
    struct Directive {
        enum class Kind : std::uint8_t {
            /// `#if`, `#ifdef` or `#ifndef`.
            If,
            /// `#elif`, `#elifdef`, `#elifndef` or `#else`.
            Else,
            /// `#endif`.
            EndIf,
            /// `#pragma region`.
            Region,
            /// `#pragma endregion`.
            EndRegion,
            /// `#include`, `#include_next` or `#import`.
            Include,
            /// `#define`.
            Define,
            /// `#pragma mark`.
            PragmaMark,
            /// `#pragma once`.
            PragmaOnce,
            /// Any other, the null directive (a `#` alone) included.
            Other,
        };

        Kind kind;

        /// From the `#` to the end of its logical line.
        LocalSourceRange range;

        /// Its tokens in LexicalInfo::tokens: the `#` at `first`, the eod
        /// ending its line at `last`.
        std::uint32_t first = 0;
        std::uint32_t last = 0;

        /// The conditional blocks open around it. An `#elif`, `#else` or
        /// `#endif` counts its own block out: it stands at its `#if`'s
        /// depth.
        std::uint32_t depth = 0;

        /// For an `#elif`, `#else` or `#endif`, the `#if` opening its
        /// block, an index into LexicalInfo::directives; none when no
        /// block is open.
        std::uint32_t opening = none;

        /// Whether it opens, continues or closes a block of lines: a
        /// conditional or a region.
        bool block() const {
            return kind == Kind::If || kind == Kind::Else || kind == Kind::EndIf ||
                   kind == Kind::Region || kind == Kind::EndRegion;
        }
    };

    /// The guard a header wraps itself in: its first directive, but for
    /// `#pragma once`, is `#ifndef X` or `#if !defined(X)`, the next one
    /// `#define X` (a value or not), and the `#endif` closing the first,
    /// with no `#else` or `#elif` between, is the last thing written.
    /// Comments and `#pragma once` are all the text outside it.
    struct IncludeGuard {
        /// The `#ifndef`, the `#define` and the `#endif`, as indices into
        /// LexicalInfo::directives.
        std::uint32_t opening;
        std::uint32_t definition;
        std::uint32_t closing;

        /// The guard macro's name in its `#define`.
        LocalSourceRange name;
    };

    /// Every token in source order, the comments and the eods ending
    /// directive lines included.
    std::vector<Token> tokens;

    /// The comments, in source order.
    std::vector<Comment> comments;

    /// The directive lines, in source order.
    std::vector<Directive> directives;

    /// Raw string literal tokens in source order, outside directives.
    std::vector<LocalSourceRange> raw_strings;

    std::vector<ModuleDeclaration> modules;

    std::optional<IncludeGuard> include_guard;

    /// The tokens of `directive` after its `#` up to its line's end, the
    /// comments left out: its name first, then its arguments.
    llvm::SmallVector<Token, 8> words(const Directive& directive) const;

    /// The index of the first token ending past `offset` — the one holding
    /// it, else the next — or the number of tokens.
    std::uint32_t token_at(std::uint32_t offset) const;

    /// The tokens from the one holding `offset` on, comments left out.
    auto code_from(std::uint32_t offset) const {
        return llvm::make_filter_range(
            llvm::ArrayRef(tokens).drop_front(token_at(offset)),
            [](const Token& token) { return token.kind != clang::tok::comment; });
    }

    /// The directive whose logical line holds `offset`, from the start of
    /// the line its `#` is on through the line's end.
    const Directive* directive_at(llvm::StringRef content, std::uint32_t offset) const;
};

/// A `#define` as its words (LexicalInfo::words) spell it.
struct MacroDefinition {
    /// The macro's name.
    Token name;

    /// A function-like macro's parameter list, its parentheses included;
    /// empty for an object-like one.
    llvm::ArrayRef<Token> parameters;

    /// The replacement list.
    llvm::ArrayRef<Token> body;
};

/// The parts of the `#define` whose words are `words`; nullopt when it
/// names no macro.
std::optional<MacroDefinition> macro_definition(llvm::ArrayRef<Token> words);

/// The kind of a directive named `name` whose first argument is
/// `argument` (`#pragma` takes its kind from it).
LexicalInfo::Directive::Kind directive_kind(llvm::StringRef name, llvm::StringRef argument);

/// Scan `content` once and collect its LexicalInfo. The matching is purely
/// lexical: module declarations are recognized by line-start grammar, so
/// consumers integrating them must cross-check compiler state (e.g. the
/// unit's named module) before trusting them.
LexicalInfo lexical_scan(llvm::StringRef content, const clang::LangOptions& dialect);

/// The comments of `content`, in source order: lexical_scan's, for a file
/// nothing else is wanted of.
std::vector<LexicalInfo::Comment> scan_comments(llvm::StringRef content,
                                                const clang::LangOptions& dialect);

/// The filename argument of the directive whose logical line holds
/// `offset`: the first filename-like token (header name, string literal,
/// or macro identifier) after a directive keyword or `__has_include`
/// operator that takes one, containing or following the offset. A
/// filename a macro's arguments pass (`#if HAS(<c.h>)`) follows no keyword
/// of its own: an `offset` at its start pins it. Nullopt off a directive,
/// or without one.
std::optional<LocalSourceRange> find_directive_argument(const LexicalInfo& info,
                                                        llvm::StringRef content,
                                                        std::uint32_t offset);

/// Bracket pairs over tokens fed in order. Brackets pair within their
/// scope — a directive line, a stretch the preprocessor skipped — and
/// outside any across the file; a closer without an opener of its kind (a
/// conditional's branches can leave both unbalanced) closes nothing, and
/// the openers a scope leaves open close with it.
class BracketPairs {
public:
    /// The opener `token` closes, if it is a closer that pairs. `skipped`
    /// tells whether the preprocessor skipped it.
    std::optional<LocalSourceRange> visit(const Token& token, bool skipped);

    /// Whether an opener is still open.
    bool inside() const {
        return !opens.empty();
    }

    bool in_directive() const {
        return directive;
    }

    bool in_skipped() const {
        return skipping;
    }

private:
    void enter();

    void leave();

    struct Open {
        TokenKind closer;
        LocalSourceRange range;
    };

    llvm::SmallVector<Open> opens;
    llvm::SmallVector<std::size_t, 2> scopes;
    bool directive = false;
    bool skipping = false;
};

}  // namespace clice
