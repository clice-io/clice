module;

#include "modules/prelude.h"

module clice:compile.tokens;

import :syntax.token;

namespace clice {

/// A top-level macro invocation written in the focus file: its spelled
/// tokens, the name through the closing parenthesis, and the tokens it
/// expanded to — none for one that expanded to nothing, a macro named in
/// a conditional directive's expression included.
struct MacroExpansion {
    llvm::ArrayRef<clang::syntax::Token> spelled;

    llvm::ArrayRef<clang::syntax::Token> expanded;
};

/// The tokens the features of an open file read, collected during its
/// compile: the focus file's spelled tokens, the expanded token stream the
/// parser consumed, and the focus file's top-level macro expansions.
///
/// Only the focus file is mapped between spelled and expanded tokens: the
/// features read the file they serve, and a header is served as the focus
/// file of its own compile. An expanded token with no spelling of its own,
/// the end-of-directive token an assembler-with-cpp `#` comment leaves
/// behind, is never recorded.
class TokenMap {
public:
    /// Starts collecting from `pp` before it enters the focus file. The map
    /// stays where it is built: the preprocessor holds on to it.
    explicit TokenMap(clang::Preprocessor& pp);

    TokenMap(const TokenMap&) = delete;
    TokenMap& operator=(const TokenMap&) = delete;

    /// Ends the collection after the parse and builds the focus file's side.
    void finish();

    /// The focus file's spelled tokens, the whole file under a preamble PCH
    /// too.
    llvm::ArrayRef<clang::syntax::Token> spelled() const {
        return spelled_tokens;
    }

    /// Where `token`, one of spelled(), starts in the focus file.
    std::uint32_t offset(const clang::syntax::Token& token) const {
        return focus_offset(token.location());
    }

    /// Per spelled token, whether it was preprocessed away to nothing: a
    /// directive, a disabled region, the preamble under a PCH, a macro
    /// invocation that expanded to nothing.
    const std::vector<bool>& preprocessed_away() const {
        return away;
    }

    /// Per raw token of the focus file, the comments and directive ends of
    /// its lexical scan among them, whether the preprocessor skipped it; a
    /// token no spelled token starts at shares the flag of the one before.
    llvm::SmallVector<bool, 0> skipped(llvm::ArrayRef<Token> tokens) const;

    /// The spelled tokens touching `location`, at most two: the one
    /// containing it or starting there, and the one ending there.
    llvm::ArrayRef<clang::syntax::Token> spelled_touching(clang::SourceLocation location) const;

    /// The spelled tokens the closed expanded token range `range` was
    /// produced from, when it maps onto them exactly: a range taking only
    /// part of a macro expansion maps to nothing, unless it lies within one
    /// argument of it.
    llvm::ArrayRef<clang::syntax::Token> spelled_for(clang::SourceRange range) const;

    /// Every token the preprocessor handed the parser, in order.
    llvm::ArrayRef<clang::syntax::Token> expanded() const {
        return expanded_tokens;
    }

    /// The expanded tokens of the closed token range `range`.
    llvm::ArrayRef<clang::syntax::Token> expanded(clang::SourceRange range) const;

    /// How many expanded(range) lookups missed the location index and
    /// searched the stream, each search a binary one by
    /// isBeforeInTranslationUnit.
    std::uint32_t searches() const {
        return search_count;
    }

    /// The focus file's top-level macro expansions whose invocation shares a
    /// token with `spelled`, a range of spelled().
    llvm::ArrayRef<MacroExpansion>
        expansions_overlapping(llvm::ArrayRef<clang::syntax::Token> spelled) const;

    /// The index into spelled() of the token expanded()[index] stands for,
    /// the one a selection of it lands on:
    ///  - a token written in the focus file: itself;
    ///  - a macro argument written in the focus file: the argument token, at
    ///    one expansion only when the macro expands it more than once;
    ///  - any other token of an expansion in the focus file: the name of the
    ///    outermost invocation.
    /// None for the rest: a header's tokens, its macros' expansions included.
    std::optional<std::uint32_t> origin(std::uint32_t index) const {
        if(index < origins_begin || index - origins_begin >= origins.size() ||
           origins[index - origins_begin] == none) {
            return std::nullopt;
        }
        return origins[index - origins_begin];
    }

private:
    struct Hooks;

    constexpr static std::uint32_t none = static_cast<std::uint32_t>(-1);

    void record(const clang::Token& token);

    void record_invocation(clang::SourceRange range);

    /// The macro argument tokens set_macro_origins meets: those the
    /// expansion getMacroArgExpandedLocation picks stands for, and the
    /// first expanded copy of each.
    struct ArgumentCopies {
        llvm::DenseSet<std::uint32_t> picked;
        llvm::DenseMap<std::uint32_t, std::uint32_t> first;
    };

    /// Sets the origins of expanded tokens [begin, end), a run of macro
    /// FileID `fid` lexed from the focus file; `name` is their invocation's.
    void set_macro_origins(clang::FileID fid,
                           std::uint32_t begin,
                           std::uint32_t end,
                           std::uint32_t name,
                           ArgumentCopies& arguments);

    bool in_focus_file(clang::SourceLocation location) const {
        return location.isFileID() && focus_begin <= location && location < focus_end;
    }

    std::uint32_t focus_offset(clang::SourceLocation location) const {
        return location.getRawEncoding() - focus_begin.getRawEncoding();
    }

    /// The index of the first spelled token at or after focus-file offset
    /// `offset`.
    std::uint32_t spelled_at_or_after(std::uint32_t offset) const;

    /// The spelled token starting at `location`, if one does.
    const clang::syntax::Token* spelled_at(clang::SourceLocation location) const;

    /// The expansion that produced expanded token `token`, if one in the
    /// focus file did.
    const MacroExpansion* expansion_of(const clang::syntax::Token& token) const;

    clang::Preprocessor& pp;
    const clang::SourceManager& SM;
    clang::FileID focus_fid;
    clang::SourceLocation focus_begin;
    clang::SourceLocation focus_end;

    std::vector<clang::syntax::Token> spelled_tokens;
    std::vector<bool> away;
    std::vector<clang::syntax::Token> expanded_tokens;

    /// Whether the preprocessor reads the focus file, not a header it
    /// includes.
    bool lexing_focus = false;

    /// The runs of expanded tokens lexed while the focus file was the file
    /// being read: its own tokens, its macro expansions, never a header's.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> focus_segments;

    /// Expanded tokens by location, for the AST's token ranges.
    llvm::DenseMap<clang::SourceLocation, std::uint32_t> expanded_index;

    mutable std::uint32_t search_count = 0;

    /// The first and last spelled token of each top-level invocation, in
    /// source order: the end of the last one bounds the next.
    std::vector<clang::SourceRange> invocations;

    /// One per invocation.
    std::vector<MacroExpansion> expansions;

    /// The expansions that produced tokens, in stream order.
    std::vector<std::uint32_t> producing;

    /// The origins of the expanded tokens from the focus file's first through
    /// its last: origins[i] is that of expanded token origins_begin + i.
    std::uint32_t origins_begin = 0;
    std::vector<std::uint32_t> origins;
};

}  // namespace clice
