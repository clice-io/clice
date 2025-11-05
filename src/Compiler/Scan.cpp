#include "Compiler/Scan.h"

namespace clice {

ScanResult scan(llvm::StringRef content) {
    ScanResult result;

    Lexer lexer{
        content,
        true,
        nullptr,
        false,
    };

    while(true) {
        auto token = lexer.advance();
        if(token.is_eof()) {
            break;
        }

        if(token.is_header_name()) {
            auto spelling = token.text(content);
            result.includes.emplace_back(spelling.starts_with('<') || spelling.ends_with('>'),
                                         /// 0,
                                         spelling.trim("<\">"));
        } else if(token.is_pp_keyword && token.text(content) == "module") {
            auto next = lexer.next();

            /// Skip module;
            if(next.kind == TokenKind::semi) {
                continue;
            }

            /// Collect module name tokens.
            while(true) {
                auto name = lexer.advance();
                if(name.is_eod()) {
                    break;
                }

                result.module_name.emplace_back(name);
            }
        }
    }

    return result;
}

}  // namespace clice
