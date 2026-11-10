module clice:feature.code_completion.snippet;

import :support.prelude;

namespace clice::feature {

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
