module;

#include "modules/prelude.h"

module clice;

import :compile.tokens;

namespace clice {

namespace {

// The spelled range in `target` covering the expanded tokens First..Last,
// which lie within one macro argument of an expansion in `target`; invalid
// when that range would also cover Prev or Next, the expanded tokens
// around them. Ported from clang's syntax::TokenBuffer.
//
// ID(ID(ID(a1) a2))
//          ~~       -> a1
//              ~~   -> a2
//       ~~~~~~~~~   -> a1 a2
clang::SourceRange spelled_within_argument(clang::SourceLocation first,
                                           clang::SourceLocation last,
                                           clang::SourceLocation prev,
                                           clang::SourceLocation next,
                                           clang::FileID target,
                                           const clang::SourceManager& SM) {
    // When First and Last are part of the same macro arg of a macro written
    // in the target file, the result is that slice of the arg, i.e. their
    // spelling range. Unwrap such macro calls: if the target file has
    // A(B(C)), the location stack of a token inside C shows the expansion of
    // A first, then B, then any macros inside C's body, then C itself.
    while(first.isMacroID() && last.isMacroID()) {
        auto decomposed_first = SM.getDecomposedLoc(first);
        auto decomposed_last = SM.getDecomposedLoc(last);
        const auto& expansion_first = SM.getSLocEntry(decomposed_first.first).getExpansion();
        const auto& expansion_last = SM.getSLocEntry(decomposed_last.first).getExpansion();

        if(!expansion_first.isMacroArgExpansion() || !expansion_last.isMacroArgExpansion()) {
            break;
        }
        // Locations are in the same macro arg if they expand to the same
        // place; an arg may still span several FileIDs.
        if(expansion_first.getExpansionLocStart() != expansion_last.getExpansionLocStart()) {
            break;
        }
        // Given `#define HIDE ID(ID(a))` and `ID(ID(HIDE))`, the token `a`
        // is wrapped in four arg expansions and only the two that expand
        // into the target file are unwrapped; those always come first.
        if(SM.getFileID(expansion_first.getExpansionLocStart()) == target) {
            break;
        }
        first = expansion_first.getSpellingLoc().getLocWithOffset(decomposed_first.second);
        last = expansion_last.getSpellingLoc().getLocWithOffset(decomposed_last.second);
    }

    // In all remaining cases the full containing macros are needed; if they
    // overlap Prev or Next, no range is possible.
    auto candidate = SM.getExpansionRange(clang::SourceRange(first, last)).getAsRange();
    auto candidate_first = SM.getDecomposedExpansionLoc(candidate.getBegin());
    auto candidate_last = SM.getDecomposedExpansionLoc(candidate.getEnd());
    // Bad input or token pasting can land in another file.
    if(candidate.isInvalid() || candidate_first.first != target || candidate_last.first != target) {
        return {};
    }
    if(prev.isValid()) {
        auto decomposed = SM.getDecomposedLoc(SM.getExpansionRange(prev).getBegin());
        if(decomposed.first != candidate_first.first ||
           decomposed.second >= candidate_first.second) {
            return {};
        }
    }
    if(next.isValid()) {
        auto decomposed = SM.getDecomposedLoc(SM.getExpansionRange(next).getEnd());
        if(decomposed.first != candidate_last.first || decomposed.second <= candidate_last.second) {
            return {};
        }
    }
    return candidate;
}

}  // namespace

struct TokenMap::Hooks : clang::PPCallbacks {
    TokenMap* map;

    explicit Hooks(TokenMap* map) : map(map) {}

    void MacroExpands(const clang::Token&,
                      const clang::MacroDefinition&,
                      clang::SourceRange range,
                      const clang::MacroArgs*) override {
        if(map) {
            map->record_invocation(range);
        }
    }
};

TokenMap::TokenMap(clang::Preprocessor& pp) : pp(pp), SM(pp.getSourceManager()) {
    auto main = SM.getMainFileID();
    main_begin = SM.getLocForStartOfFile(main);
    main_end = SM.getLocForEndOfFile(main);
    pp.setTokenWatcher([this](const clang::Token& token) { record(token); });
    auto owned = std::make_unique<Hooks>(this);
    hooks = owned.get();
    pp.addPPCallbacks(std::move(owned));
}

void TokenMap::record(const clang::Token& token) {
    if(token.is(clang::tok::annot_module_name)) {
        auto range = clang::CharSourceRange::getTokenRange(token.getAnnotationRange());
        auto text = clang::Lexer::getSourceText(range, SM, pp.getLangOpts());
        expanded_tokens.emplace_back(token.getLocation(),
                                     static_cast<unsigned>(text.size()),
                                     token.getKind());
        return;
    }
    if(token.isAnnotation() || token.is(clang::tok::eod)) {
        return;
    }
    expanded_tokens.emplace_back(token);
}

void TokenMap::record_invocation(clang::SourceRange range) {
    // A top-level invocation ends in the file; one ending within the last
    // one sits in its arguments.
    auto end = range.getEnd();
    if(!in_main_file(end) || (!invocations.empty() && end <= invocations.back().end)) {
        return;
    }
    auto begin = range.getBegin();
    if(begin.isMacroID()) {
        // `#define A 1 + B` used as `A(2)`: B's name comes from A's
        // expansion and its arguments from the file, so A's invocation
        // grows to take B's in.
        begin = SM.getExpansionLoc(begin);
        if(!invocations.empty() && invocations.back().begin == begin) {
            invocations.back().end = end;
        }
        return;
    }
    invocations.push_back({begin, end, static_cast<std::uint32_t>(expanded_tokens.size())});
}

void TokenMap::finish() {
    pp.setTokenWatcher(nullptr);
    hooks->map = nullptr;

    spelled_tokens = clang::syntax::tokenize(SM.getMainFileID(), SM, pp.getLangOpts());

    expanded_index.reserve(expanded_tokens.size());
    for(auto [index, token]: llvm::enumerate(expanded_tokens)) {
        expanded_index[token.location()] = static_cast<std::uint32_t>(index);
    }

    llvm::ArrayRef<clang::syntax::Token> spelled = spelled_tokens;
    llvm::ArrayRef<clang::syntax::Token> stream = expanded_tokens;
    expansions.reserve(invocations.size());
    for(auto& invocation: invocations) {
        auto first = spelled_at_or_after(main_offset(invocation.begin));
        auto last = spelled_at_or_after(main_offset(invocation.end) + 1);
        auto end = invocation.first_expanded;
        while(end < stream.size() && stream[end].location().isMacroID() &&
              SM.getExpansionLoc(stream[end].location()) == invocation.begin) {
            end += 1;
        }
        expansions.push_back({
            .spelled = spelled.slice(first, last - first),
            .expanded = stream.slice(invocation.first_expanded, end - invocation.first_expanded),
        });
    }

    // A spelled token survives preprocessing when the parser saw it or it
    // is part of an invocation that expanded to something.
    away.assign(spelled_tokens.size(), true);
    for(auto& token: expanded_tokens) {
        if(in_main_file(token.location())) {
            auto index = spelled_at_or_after(main_offset(token.location()));
            if(index < spelled.size() && spelled[index].location() == token.location()) {
                away[index] = false;
            }
        }
    }
    for(auto& expansion: expansions) {
        if(!expansion.expanded.empty()) {
            for(auto& token: expansion.spelled) {
                away[&token - spelled.data()] = false;
            }
        }
    }
}

std::uint32_t TokenMap::spelled_at_or_after(std::uint32_t offset) const {
    auto it = llvm::partition_point(spelled_tokens, [&](const clang::syntax::Token& token) {
        return main_offset(token.location()) < offset;
    });
    return static_cast<std::uint32_t>(it - spelled_tokens.begin());
}

const MacroExpansion* TokenMap::expansion_of(std::uint32_t index) const {
    const auto* token = &expanded_tokens[index];
    auto it = llvm::partition_point(expansions, [&](const MacroExpansion& expansion) {
        return expansion.expanded.begin() <= token;
    });
    if(it == expansions.begin()) {
        return nullptr;
    }
    it -= 1;
    return token < it->expanded.end() ? &*it : nullptr;
}

llvm::ArrayRef<clang::syntax::Token>
    TokenMap::spelled_touching(clang::SourceLocation location) const {
    // The end of the file still touches its last token.
    if(!location.isFileID() || location < main_begin || main_end < location) {
        return {};
    }
    return clang::syntax::spelledTokensTouching(location, spelled_tokens);
}

llvm::ArrayRef<clang::syntax::Token> TokenMap::spelled_for(clang::SourceRange range) const {
    auto tokens = expanded(range);
    // Ranges of invalid code can take in the eof, which has no spelling.
    if(!tokens.empty() && tokens.back().kind() == clang::tok::eof) {
        tokens = tokens.drop_back();
    }
    if(tokens.empty()) {
        return {};
    }
    const auto& first = tokens.front();
    const auto& last = tokens.back();
    const auto* first_expansion =
        expansion_of(static_cast<std::uint32_t>(&first - expanded_tokens.data()));
    const auto* last_expansion =
        expansion_of(static_cast<std::uint32_t>(&last - expanded_tokens.data()));

    llvm::ArrayRef<clang::syntax::Token> spelled = spelled_tokens;
    if(first_expansion && first_expansion == last_expansion &&
       SM.isMacroArgExpansion(first.location()) && SM.isMacroArgExpansion(last.location())) {
        auto prev =
            &first == expanded_tokens.data() ? clang::SourceLocation() : (&first - 1)->location();
        auto next =
            &last == &expanded_tokens.back() ? clang::SourceLocation() : (&last + 1)->location();
        auto covering = spelled_within_argument(first.location(),
                                                last.location(),
                                                prev,
                                                next,
                                                SM.getMainFileID(),
                                                SM);
        if(covering.isInvalid()) {
            return {};
        }
        auto begin = spelled_at_or_after(main_offset(covering.getBegin()));
        auto end = spelled_at_or_after(main_offset(covering.getEnd()) + 1);
        return spelled.slice(begin, end - begin);
    }

    // Anything else maps whole expansions, or file tokens of the main file.
    auto spelled_of = [&](const clang::syntax::Token& token) -> const clang::syntax::Token* {
        if(!in_main_file(token.location())) {
            return nullptr;
        }
        auto index = spelled_at_or_after(main_offset(token.location()));
        if(index == spelled.size() || spelled[index].location() != token.location()) {
            return nullptr;
        }
        return &spelled[index];
    };
    const clang::syntax::Token* begin = nullptr;
    if(first_expansion) {
        if(&first != first_expansion->expanded.begin()) {
            return {};
        }
        begin = first_expansion->spelled.begin();
    } else {
        begin = spelled_of(first);
    }
    const clang::syntax::Token* end = nullptr;
    if(last_expansion) {
        if(&last + 1 != last_expansion->expanded.end()) {
            return {};
        }
        end = last_expansion->spelled.end();
    } else if(const auto* token = spelled_of(last)) {
        end = token + 1;
    }
    if(!begin || !end || begin >= end) {
        return {};
    }
    return {begin, end};
}

llvm::ArrayRef<clang::syntax::Token> TokenMap::expanded(clang::SourceRange range) const {
    if(range.isInvalid()) {
        return {};
    }
    // The AST's ranges start and end at expanded tokens.
    auto begin = expanded_index.find(range.getBegin());
    auto end = expanded_index.find(range.getEnd());
    if(begin != expanded_index.end() && end != expanded_index.end()) {
        if(begin->second > end->second + 1) {
            return {};
        }
        return llvm::ArrayRef(expanded_tokens)
            .slice(begin->second, end->second + 1 - begin->second);
    }
    // The rest, a split `>>` for one, binary-search the stream.
    llvm::ArrayRef<clang::syntax::Token> stream = expanded_tokens;
    const auto* first = llvm::partition_point(stream, [&](const clang::syntax::Token& token) {
        return SM.isBeforeInTranslationUnit(token.location(), range.getBegin());
    });
    const auto* last = llvm::partition_point(stream, [&](const clang::syntax::Token& token) {
        return !SM.isBeforeInTranslationUnit(range.getEnd(), token.location());
    });
    if(first > last) {
        return {};
    }
    return {first, last};
}

llvm::ArrayRef<MacroExpansion>
    TokenMap::expansions_overlapping(llvm::ArrayRef<clang::syntax::Token> spelled) const {
    if(spelled.empty()) {
        return {};
    }
    auto first = llvm::partition_point(expansions, [&](const MacroExpansion& expansion) {
        return expansion.spelled.end() <= &spelled.front();
    });
    auto last = llvm::partition_point(expansions, [&](const MacroExpansion& expansion) {
        return expansion.spelled.begin() <= &spelled.back();
    });
    if(first >= last) {
        return {};
    }
    return {&*first, static_cast<std::size_t>(last - first)};
}

}  // namespace clice
