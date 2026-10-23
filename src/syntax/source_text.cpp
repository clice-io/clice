module;

#include "support/prelude.h"

module clice;

import :syntax.lexer;
import :syntax.lexical_scan;
import :syntax.position;
import :syntax.source_text;

namespace clice {

void SourceText::set_dialect(const clang::LangOptions& dialect) {
    if(options == &dialect) {
        return;
    }
    options = &dialect;
    table = &keywords(dialect);
    scan.reset();
    comment_scan.reset();
}

auto SourceText::positions() -> PositionMap {
    if(!lines) {
        std::string_view text(content);
        lines = LineTables{
            .starts = lsp::line_starts(text),
            .non_ascii = lsp::non_ascii_lines(text),
        };
    }
    return {content, lines->starts, lines->non_ascii};
}

auto SourceText::lexical() -> const LexicalInfo& {
    if(!scan) {
        scan = lexical_scan(content, dialect());
    }
    return *scan;
}

auto SourceText::comments() -> llvm::ArrayRef<LexicalInfo::Comment> {
    if(scan) {
        return scan->comments;
    }
    if(!comment_scan) {
        comment_scan = scan_comments(content, dialect());
    }
    return *comment_scan;
}

}  // namespace clice
