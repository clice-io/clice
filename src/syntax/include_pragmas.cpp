module clice;

import :syntax.include_pragmas;
import :syntax.lexer;
import :syntax.token;

namespace clice {

IncludePragmas include_pragmas(llvm::StringRef text, const clang::LangOptions& dialect) {
    IncludePragmas pragmas;
    if(!text.contains("IWYU pragma:")) {
        return pragmas;
    }
    Lexer lexer(text, dialect, {.keep_comments = true});
    bool exporting = false;
    bool in_include = false;
    /// The header name of the include on the line lexed, until a newline.
    std::optional<std::string> included;
    std::uint32_t included_end = 0;
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        if(token.is_directive_hash()) {
            in_include = false;
            included.reset();
        } else if(token.is_pp_keyword) {
            in_include = takes_header_name(token.text(text));
        } else if(in_include && token.is_header_name()) {
            auto name = token.text(text);
            included = name.size() >= 2 ? name.drop_front().drop_back().str() : std::string();
            included_end = token.range.end;
            if(exporting) {
                pragmas.exports.push_back(*included);
            }
            in_include = false;
        }
        if(token.kind != clang::tok::comment) {
            continue;
        }
        auto comment = token.text(text);
        auto at = comment.find("IWYU pragma:");
        if(at == llvm::StringRef::npos) {
            continue;
        }
        auto pragma = comment.drop_front(at + llvm::StringRef("IWYU pragma:").size()).trim();
        pragma.consume_back("*/");
        pragma = pragma.rtrim();
        if(pragma.consume_front("private")) {
            pragmas.is_private = true;
            auto rest = pragma.ltrim();
            if(rest.consume_front(",")) {
                rest = rest.ltrim();
                if(rest.consume_front("include")) {
                    pragmas.include_instead = rest.trim().str();
                }
            }
        } else if(pragma == "begin_exports") {
            exporting = true;
        } else if(pragma == "end_exports") {
            exporting = false;
        } else if(pragma == "export" && included &&
                  !text.slice(included_end, token.range.begin).contains('\n')) {
            pragmas.exports.push_back(*included);
        }
    }
    return pragmas;
}

}  // namespace clice
