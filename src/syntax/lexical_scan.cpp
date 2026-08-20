#include "syntax/lexical_scan.h"

#include "syntax/lexer.h"

namespace clice {

LexicalInfo lexical_scan(llvm::StringRef content, const clang::LangOptions* lang_opts) {
    using Comment = LexicalInfo::Comment;
    using ModuleDeclaration = LexicalInfo::ModuleDeclaration;

    LexicalInfo info;
    Lexer lexer(content, {.keep_comments = true, .lang_opts = lang_opts});

    auto record_comment = [&](const Token& token) {
        auto kind =
            token.text(content).starts_with("//") ? Comment::Kind::Line : Comment::Kind::Block;
        info.comments.push_back({kind, token.range});
    };

    // A comment-transparent pull interface: module declaration grammar must
    // match across interleaved comments, which are recorded on the way.
    auto advance = [&] {
        while(true) {
            auto token = lexer.advance();
            if(token.kind != clang::tok::comment) {
                return token;
            }
            record_comment(token);
        }
    };

    auto peek = [&] {
        while(true) {
            auto token = lexer.next();
            if(token.kind != clang::tok::comment) {
                return token;
            }
            lexer.advance();
            record_comment(token);
        }
    };

    auto advance_if = [&](llvm::function_ref<bool(const Token&)> pred) -> std::optional<Token> {
        if(auto token = peek(); pred(token)) {
            return advance();
        }
        return std::nullopt;
    };

    auto advance_if_kind = [&](TokenKind kind) {
        return advance_if([&](const Token& token) { return token.kind == kind; });
    };

    auto is_spelled = [&](const Token& token, llvm::StringRef spelling) {
        return token.is_identifier() && token.text(content) == spelling;
    };

    auto lex_dotted = [&](llvm::SmallVectorImpl<LocalSourceRange>& parts) {
        while(true) {
            auto part = advance_if([](const Token& token) { return token.is_identifier(); });
            if(!part) {
                return;
            }
            parts.push_back(part->range);
            if(!advance_if_kind(clang::tok::period)) {
                return;
            }
        }
    };

    bool file_start = true;

    while(true) {
        auto token = advance();
        if(token.is_eof()) {
            break;
        }

        bool at_file_start = file_start;
        file_start = false;

        // Valid code cannot begin a logical line with `module` (or `export
        // module`) in any other meaning, so line-start matching is exact;
        // stray matches in invalid code are filtered by the consumers'
        // compiler-state cross-checks.
        if(!token.is_at_start_of_line || !token.is_identifier()) {
            continue;
        }

        ModuleDeclaration decl;
        if(is_spelled(token, "export")) {
            if(!is_spelled(peek(), "module")) {
                continue;
            }
            decl.export_keyword = token.range;
            token = advance();
        } else if(!is_spelled(token, "module")) {
            continue;
        }
        decl.keyword = token.range;

        // `module;` introduces the global module fragment only as the
        // file's first token; a bare `module;` anywhere else is nothing.
        if(peek().kind == clang::tok::semi) {
            if(at_file_start && !decl.export_keyword.valid()) {
                decl.kind = ModuleDeclaration::Kind::GlobalFragment;
                info.modules.push_back(std::move(decl));
            }
            continue;
        }

        // `module :private;` — the private fragment takes no export.
        if(auto colon = advance_if_kind(clang::tok::colon)) {
            decl.colon = colon->range;
            auto is_private = [&](const Token& token) {
                return is_spelled(token, "private");
            };
            if(auto private_keyword = advance_if(is_private);
               private_keyword && !decl.export_keyword.valid()) {
                decl.kind = ModuleDeclaration::Kind::PrivateFragment;
                decl.partition_parts.push_back(private_keyword->range);
                info.modules.push_back(std::move(decl));
            }
            continue;
        }

        lex_dotted(decl.name_parts);
        if(decl.name_parts.empty()) {
            continue;
        }

        if(auto colon = advance_if_kind(clang::tok::colon)) {
            decl.colon = colon->range;
            lex_dotted(decl.partition_parts);
        }

        decl.kind = ModuleDeclaration::Kind::Declaration;
        info.modules.push_back(std::move(decl));
    }

    return info;
}

}  // namespace clice
