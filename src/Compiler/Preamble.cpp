#include "Compiler/Preamble.h"
#include "AST/SourceCode.h"
#include "Support/Format.h"
#include "Support/Logging.h"

namespace clice {

std::uint32_t compute_preamble_bound(llvm::StringRef content) {
    auto result = compute_preamble_bounds(content);
    if(result.empty()) {
        return 0;
    } else {
        return result.back();
    }
}

std::vector<std::uint32_t> compute_preamble_bounds(llvm::StringRef content) {
    std::vector<std::uint32_t> result;

    Lexer lexer(content, true, nullptr, false);

    while(true) {
        auto token = lexer.advance();
        if(token.is_eof()) {
            break;
        }

        if(token.is_at_start_of_line) {
            if(token.kind == clang::tok::hash) {
                /// For preprocessor directive, consume the whole directive.
                lexer.advance_until(clang::tok::eod);
                auto last = lexer.last();

                /// Append the token before the eod.
                result.push_back(last.range.end);
            } else if(token.is_identifier() && token.text(content) == "module") {
                /// If we encounter a module keyword at the start of a line, it may be
                /// a module declaration or global module fragment.
                auto next = lexer.next();

                if(next.kind == clang::tok::semi) {
                    /// If next token is `;`, it is a global module fragment.
                    /// we just continue.
                    lexer.advance();

                    /// Append it to bounds.
                    result.push_back(next.range.end);
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    }

    return result;
}

}  // namespace clice
