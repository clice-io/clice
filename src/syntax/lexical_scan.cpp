module;

#include "modules/prelude.h"

module clice;

import :syntax.lexer;
import :syntax.lexical_scan;

namespace clice {

static LexicalInfo::Comment comment_of(llvm::StringRef content, const Token& token) {
    using Kind = LexicalInfo::Comment::Kind;
    return {token.text(content).starts_with("//") ? Kind::Line : Kind::Block, token.range};
}

LexicalInfo lexical_scan(llvm::StringRef content, const clang::LangOptions* lang_opts) {
    using ModuleDeclaration = LexicalInfo::ModuleDeclaration;
    using BlockDirective = LexicalInfo::BlockDirective;

    LexicalInfo info;
    Lexer lexer(content, {.keep_comments = true, .lang_opts = lang_opts});

    auto record_comment = [&](const Token& token) {
        info.comments.push_back(comment_of(content, token));
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

    auto scan_directive = [&](const Token& hash) {
        auto token = advance();
        auto keyword = token.text(content);
        std::optional<BlockDirective::Kind> kind;
        std::optional<std::uint32_t> mark_text_begin;
        if(keyword == "if" || keyword == "ifdef" || keyword == "ifndef") {
            kind = BlockDirective::Kind::If;
        } else if(keyword == "elif" || keyword == "elifdef" || keyword == "elifndef" ||
                  keyword == "else") {
            kind = BlockDirective::Kind::Else;
        } else if(keyword == "endif") {
            kind = BlockDirective::Kind::EndIf;
        } else if(keyword == "pragma") {
            // Only the first argument decides: `#pragma mark endregion`
            // closes nothing.
            token = advance();
            if(is_spelled(token, "region")) {
                kind = BlockDirective::Kind::Region;
            } else if(is_spelled(token, "endregion")) {
                kind = BlockDirective::Kind::EndRegion;
            } else if(is_spelled(token, "mark")) {
                mark_text_begin = token.range.end;
            }
        }
        while(!token.is_eod() && !token.is_eof()) {
            token = advance();
        }
        LocalSourceRange range{hash.range.begin, token.range.begin};
        if(kind) {
            info.block_directives.push_back({.kind = *kind, .range = range});
        } else if(keyword == "include" || keyword == "include_next" || keyword == "import") {
            info.include_directives.push_back(range);
        } else if(keyword == "define") {
            info.macro_definitions.push_back(range);
        } else if(mark_text_begin) {
            info.pragma_marks.push_back({
                .range = range,
                .text = {*mark_text_begin, range.end}
            });
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

        if(token.is_directive_hash()) {
            scan_directive(token);
            continue;
        }

        if(clang::tok::isStringLiteral(token.kind) &&
           token.text(content).split('"').first.ends_with("R")) {
            info.raw_strings.push_back(token.range);
            continue;
        }

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

std::vector<LexicalInfo::Comment> scan_comments(llvm::StringRef content,
                                                const clang::LangOptions* lang_opts) {
    std::vector<LexicalInfo::Comment> comments;
    Lexer lexer(content, {.keep_comments = true, .lang_opts = lang_opts});
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        if(token.kind == clang::tok::comment) {
            comments.push_back(comment_of(content, token));
        }
    }
    return comments;
}

}  // namespace clice
