module;

#include "support/prelude.h"

/// The inside of code completion its two sides share: what a compile
/// completes (compile.cpp) and what the server completes without one
/// (project.cpp) match, rank and spell their items alike.

module clice:feature.code_completion.item;

import :syntax.lexer;
import :syntax.token;

namespace clice::feature {

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

/// How strongly the context asks for a candidate, apart from how its name
/// matches: clang's priority — locals before members before the rest, a
/// type the context expects ahead of all, macros last — as a factor around
/// 1, the priority of a keyword.
inline float priority_weight(unsigned priority) {
    return std::max(0.05F, (90.0F - static_cast<float>(priority)) / 50.0F);
}

/// Ascending sort text for a candidate scoring `score`, at most 8; `order`
/// breaks ties.
inline std::string sort_text(float score, std::optional<std::uint32_t> order) {
    auto text = std::format("{:.4f}", 8.0F - score);
    if(order) {
        text += std::format("{:05}", *order);
    }
    return text;
}

/// Text a snippet takes literally: `$` and `\` would otherwise start or
/// escape a snippet construct, and inside a placeholder `}` would end it.
inline auto escape_snippet(llvm::StringRef text, bool placeholder = false) -> std::string {
    std::string escaped;
    for(char c: text) {
        if(c == '$' || c == '\\' || (placeholder && c == '}')) {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

}  // namespace clice::feature
