module;

#include "modules/prelude.h"

module clice:compile.tokens;

namespace clice {

/// A top-level macro invocation written in the main file: its spelled
/// tokens, the name through the closing parenthesis, and the tokens it
/// expanded to — none for one that expanded to nothing, a macro named in
/// a conditional directive's expression included.
struct MacroExpansion {
    llvm::ArrayRef<clang::syntax::Token> spelled;

    llvm::ArrayRef<clang::syntax::Token> expanded;
};

/// The tokens the features of an open file read, collected during its
/// compile: the main file's spelled tokens, the expanded token stream the
/// parser consumed, and the main file's top-level macro expansions.
///
/// Only the main file is mapped between spelled and expanded tokens: the
/// features read the file they serve, and a header is served as the main
/// file of its own compile. An expanded token with no spelling of its own,
/// the end-of-directive token an assembler-with-cpp `#` comment leaves
/// behind, is never recorded.
class TokenMap {
public:
    /// Starts collecting from `pp`, which has entered the main file and not
    /// lexed it yet. The map stays where it is built: the preprocessor
    /// holds on to it until finish().
    explicit TokenMap(clang::Preprocessor& pp);

    TokenMap(const TokenMap&) = delete;
    TokenMap& operator=(const TokenMap&) = delete;

    /// Ends the collection after the parse and builds the main file's side.
    void finish();

    /// The main file's spelled tokens, the whole file under a preamble PCH
    /// too.
    llvm::ArrayRef<clang::syntax::Token> spelled() const {
        return spelled_tokens;
    }

    /// Per spelled token, whether it was preprocessed away to nothing: a
    /// directive, a disabled region, the preamble under a PCH, a macro
    /// invocation that expanded to nothing.
    const std::vector<bool>& preprocessed_away() const {
        return away;
    }

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

    /// The main file's top-level macro expansions whose invocation shares a
    /// token with `spelled`, a range of spelled().
    llvm::ArrayRef<MacroExpansion>
        expansions_overlapping(llvm::ArrayRef<clang::syntax::Token> spelled) const;

private:
    struct Hooks;

    /// An invocation the preprocessor reported: its first and last spelled
    /// token, and where its expanded tokens start in the stream.
    struct Invocation {
        clang::SourceLocation begin;
        clang::SourceLocation end;
        std::uint32_t first_expanded;
    };

    void record(const clang::Token& token);

    void record_invocation(clang::SourceRange range);

    bool in_main_file(clang::SourceLocation location) const {
        return location.isFileID() && main_begin <= location && location < main_end;
    }

    std::uint32_t main_offset(clang::SourceLocation location) const {
        return location.getRawEncoding() - main_begin.getRawEncoding();
    }

    /// The index of the first spelled token at or after main-file offset
    /// `offset`.
    std::uint32_t spelled_at_or_after(std::uint32_t offset) const;

    /// The expansion that produced expanded token `index`, if one in the
    /// main file did.
    const MacroExpansion* expansion_of(std::uint32_t index) const;

    clang::Preprocessor& pp;
    const clang::SourceManager& SM;
    clang::SourceLocation main_begin;
    clang::SourceLocation main_end;

    /// Owned by the preprocessor; cut off from this map at finish().
    Hooks* hooks;

    std::vector<clang::syntax::Token> spelled_tokens;
    std::vector<bool> away;
    std::vector<clang::syntax::Token> expanded_tokens;

    /// Expanded tokens by location, for the AST's token ranges.
    llvm::DenseMap<clang::SourceLocation, std::uint32_t> expanded_index;

    /// Top-level, in source order: the end of the last one bounds the next.
    std::vector<Invocation> invocations;
    std::vector<MacroExpansion> expansions;
};

}  // namespace clice
