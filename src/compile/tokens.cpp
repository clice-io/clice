module;

#include "modules/prelude.h"

module clice;

import :compile.tokens;

namespace clice {

// The main-file range covering the expanded tokens `first`..`last`, which
// lie within one macro argument of an expansion in the main file; invalid
// when that range would also cover `prev` or `next`, the expanded tokens
// around them. Ported from clang's syntax::TokenBuffer.
//
// ID(ID(ID(a1) a2))
//          ~~       -> a1
//              ~~   -> a2
//       ~~~~~~~~~   -> a1 a2
static clang::SourceRange spelled_within_argument(clang::SourceLocation first,
                                                  clang::SourceLocation last,
                                                  clang::SourceLocation prev,
                                                  clang::SourceLocation next,
                                                  const clang::SourceManager& SM) {
    auto target = SM.getMainFileID();
    // When `first` and `last` are part of the same macro arg of a macro
    // written in the main file, the result is that slice of the arg, i.e.
    // their spelling range. Unwrap such macro calls: if the main file has
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
    // overlap `prev` or `next`, no range is possible.
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

struct TokenMap::Hooks : clang::PPCallbacks {
    TokenMap& map;

    explicit Hooks(TokenMap& map) : map(map) {}

    void MacroExpands(const clang::Token&,
                      const clang::MacroDefinition&,
                      clang::SourceRange range,
                      const clang::MacroArgs*) override {
        map.record_invocation(range);
    }

    void LexedFileChanged(clang::FileID fid,
                          LexedFileChangeReason,
                          clang::SrcMgr::CharacteristicKind,
                          clang::FileID,
                          clang::SourceLocation) override {
        map.lexing_main = fid == map.main_fid;
    }
};

TokenMap::TokenMap(clang::Preprocessor& pp) : pp(pp), SM(pp.getSourceManager()) {
    main_fid = SM.getMainFileID();
    main_begin = SM.getLocForStartOfFile(main_fid);
    main_end = SM.getLocForEndOfFile(main_fid);
    pp.setTokenWatcher([this](const clang::Token& token) { record(token); });
    pp.addPPCallbacks(std::make_unique<Hooks>(*this));
}

void TokenMap::record(const clang::Token& token) {
    if(token.isAnnotation() && !token.is(clang::tok::annot_module_name)) {
        return;
    }
    if(token.is(clang::tok::eod)) {
        return;
    }
    auto index = static_cast<std::uint32_t>(expanded_tokens.size());
    if(lexing_main) {
        if(main_segments.empty() || main_segments.back().second != index) {
            main_segments.emplace_back(index, index);
        }
        main_segments.back().second += 1;
    }
    if(token.is(clang::tok::annot_module_name)) {
        auto range = clang::CharSourceRange::getTokenRange(token.getAnnotationRange());
        auto text = clang::Lexer::getSourceText(range, SM, pp.getLangOpts());
        expanded_tokens.emplace_back(token.getLocation(),
                                     static_cast<unsigned>(text.size()),
                                     token.getKind());
    } else {
        expanded_tokens.emplace_back(token);
    }
}

void TokenMap::record_invocation(clang::SourceRange range) {
    // A top-level invocation ends in the file; one ending within the last
    // one sits in its arguments.
    auto end = range.getEnd();
    if(!in_main_file(end) || (!invocations.empty() && end <= invocations.back().getEnd())) {
        return;
    }
    auto begin = range.getBegin();
    if(begin.isMacroID()) {
        // `#define A 1 + B` used as `A(2)`: B's name comes from A's
        // expansion and its arguments from the file, so A's invocation
        // grows to take B's in.
        begin = SM.getExpansionLoc(begin);
        if(!invocations.empty() && invocations.back().getBegin() == begin) {
            invocations.back().setEnd(end);
        }
        return;
    }
    invocations.emplace_back(begin, end);
}

void TokenMap::finish() {
    spelled_tokens = clang::syntax::tokenize(main_fid, SM, pp.getLangOpts());

    // Every token, a header's included: the main file's nodes have ranges
    // in headers too, and a miss costs a search by isBeforeInTranslationUnit.
    expanded_index.reserve(expanded_tokens.size());
    for(auto [index, token]: llvm::enumerate(expanded_tokens)) {
        expanded_index[token.location()] = static_cast<std::uint32_t>(index);
    }

    // An invocation's expanded tokens are the run that expands from its
    // name, wherever it lands in the stream: pragma handlers lex theirs
    // ahead and hand them over later, and a builtin like
    // `__has_cpp_attribute` expands the macros of its argument first.
    llvm::DenseMap<clang::SourceLocation, std::uint32_t> by_begin;
    for(auto [index, invocation]: llvm::enumerate(invocations)) {
        by_begin[invocation.getBegin()] = static_cast<std::uint32_t>(index);
    }
    llvm::ArrayRef<clang::syntax::Token> stream = expanded_tokens;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs(invocations.size());
    for(auto [segment_begin, segment_end]: main_segments) {
        for(auto index = segment_begin; index < segment_end;) {
            auto location = stream[index].location();
            if(location.isFileID()) {
                index += 1;
                continue;
            }
            // The tokens of one macro FileID share where they expand from.
            auto fid = SM.getFileID(location);
            auto limit = SM.getComposedLoc(fid, SM.getFileIDSize(fid));
            auto end = index + 1;
            while(end < segment_end && location <= stream[end].location() &&
                  stream[end].location() <= limit) {
                end += 1;
            }
            if(auto it = by_begin.find(SM.getExpansionLoc(location)); it != by_begin.end()) {
                auto& run = runs[it->second];
                if(run.second == 0) {
                    run = {index, end};
                } else if(run.second == index) {
                    run.second = end;
                }
            }
            index = end;
        }
    }

    llvm::ArrayRef<clang::syntax::Token> spelled = spelled_tokens;
    expansions.reserve(invocations.size());
    for(auto [invocation, run]: llvm::zip_equal(invocations, runs)) {
        auto first = spelled_at_or_after(main_offset(invocation.getBegin()));
        auto last = spelled_at_or_after(main_offset(invocation.getEnd()) + 1);
        expansions.push_back({
            .spelled = spelled.slice(first, last - first),
            .expanded = stream.slice(run.first, run.second - run.first),
        });
    }
    for(auto [index, expansion]: llvm::enumerate(expansions)) {
        if(!expansion.expanded.empty()) {
            producing.push_back(static_cast<std::uint32_t>(index));
        }
    }
    llvm::sort(producing, [&](std::uint32_t left, std::uint32_t right) {
        return expansions[left].expanded.begin() < expansions[right].expanded.begin();
    });

    // A spelled token survives preprocessing when the parser saw it or it
    // is part of an invocation that expanded to something.
    away.assign(spelled_tokens.size(), true);
    for(auto [begin, end]: main_segments) {
        for(auto& token: stream.slice(begin, end - begin)) {
            if(const auto* written = spelled_at(token.location())) {
                away[written - spelled.data()] = false;
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

const clang::syntax::Token* TokenMap::spelled_at(clang::SourceLocation location) const {
    if(!in_main_file(location)) {
        return nullptr;
    }
    auto index = spelled_at_or_after(main_offset(location));
    if(index == spelled_tokens.size() || spelled_tokens[index].location() != location) {
        return nullptr;
    }
    return &spelled_tokens[index];
}

const MacroExpansion* TokenMap::expansion_of(const clang::syntax::Token& token) const {
    auto it = llvm::partition_point(producing, [&](std::uint32_t index) {
        return expansions[index].expanded.begin() <= &token;
    });
    if(it == producing.begin()) {
        return nullptr;
    }
    const auto& expansion = expansions[*std::prev(it)];
    return &token < expansion.expanded.end() ? &expansion : nullptr;
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
    const auto* first_expansion = expansion_of(first);
    const auto* last_expansion = expansion_of(last);

    llvm::ArrayRef<clang::syntax::Token> spelled = spelled_tokens;
    if(first_expansion && first_expansion == last_expansion &&
       SM.isMacroArgExpansion(first.location()) && SM.isMacroArgExpansion(last.location())) {
        auto prev =
            &first == expanded_tokens.data() ? clang::SourceLocation() : (&first - 1)->location();
        auto next =
            &last == &expanded_tokens.back() ? clang::SourceLocation() : (&last + 1)->location();
        auto covering = spelled_within_argument(first.location(), last.location(), prev, next, SM);
        if(covering.isInvalid()) {
            return {};
        }
        auto begin = spelled_at_or_after(main_offset(covering.getBegin()));
        auto end = spelled_at_or_after(main_offset(covering.getEnd()) + 1);
        return spelled.slice(begin, end - begin);
    }

    // Anything else maps whole expansions, or file tokens of the main file.
    const clang::syntax::Token* begin = nullptr;
    if(first_expansion) {
        if(&first != first_expansion->expanded.begin()) {
            return {};
        }
        begin = first_expansion->spelled.begin();
    } else {
        begin = spelled_at(first.location());
    }
    const clang::syntax::Token* end = nullptr;
    if(last_expansion) {
        if(&last + 1 != last_expansion->expanded.end()) {
            return {};
        }
        end = last_expansion->spelled.end();
    } else if(const auto* token = spelled_at(last.location())) {
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
    llvm::ArrayRef<MacroExpansion> all = expansions;
    const auto* first = llvm::partition_point(all, [&](const MacroExpansion& expansion) {
        return expansion.spelled.end() <= &spelled.front();
    });
    const auto* last = llvm::partition_point(all, [&](const MacroExpansion& expansion) {
        return expansion.spelled.begin() <= &spelled.back();
    });
    return {first, last};
}

}  // namespace clice
