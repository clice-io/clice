module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :feature.code_action.action;

namespace clice::feature::action {

namespace {

/// Code as a fix's title shows it: its first line past the line breaks it
/// starts with, cut short.
std::string title_code(llvm::StringRef code) {
    if(code == "\n") {
        return R"(\n)";
    }
    code = code.ltrim("\r\n");
    auto shown = code.split('\n').first.take_front(50);
    return shown.size() == code.size() ? shown.str() : std::format("{}…", shown);
}

/// clangd's title for a one-edit fix — "change 'a' to 'b'", "remove 'a'",
/// "insert 'b'" — else the diagnostic's message.
std::string fix_title(llvm::StringRef content, const Diagnostic& diagnostic) {
    if(diagnostic.fix.size() != 1) {
        return diagnostic.message;
    }
    const auto& edit = diagnostic.fix.front();
    auto removed = content.substr(edit.range.begin, edit.range.length());
    std::string title;
    if(!removed.empty() && !edit.text.empty()) {
        title = std::format("change '{}' to '{}'", title_code(removed), title_code(edit.text));
    } else if(!removed.empty()) {
        title = std::format("remove '{}'", title_code(removed));
    } else if(!edit.text.empty()) {
        title = std::format("insert '{}'", title_code(edit.text));
    } else {
        return diagnostic.message;
    }
    std::ranges::replace(title, '\n', ' ');
    return title;
}

/// A fix's edits as the main file takes them. clang-tidy inserts an
/// include into a file without one at its first byte, which in a module
/// unit is before its declaration, where no directive may go: there it
/// goes where an added include goes.
std::vector<TextReplacement> placed(CompilationUnit& unit, llvm::ArrayRef<TextReplacement> fix) {
    std::vector<TextReplacement> edits(fix.begin(), fix.end());
    if(!unit.is_named_module()) {
        return edits;
    }
    for(auto& edit: edits) {
        llvm::StringRef header = edit.text;
        if(edit.range.begin == 0 && edit.range.length() == 0 && header.consume_front("#include ")) {
            auto insertion = include_insertion(unit);
            edit = {
                {insertion.offset, insertion.offset},
                insertion.text(header.trim())
            };
        }
    }
    return edits;
}

}  // namespace

void fix_its(CompilationUnit& unit, LocalSourceRange selection, std::vector<CodeAction>& out) {
    auto content = unit.focus_file().content();
    for(const auto& main: unit.diagnostics()) {
        if(!unit.focus_file().contains(main.fid) || !main.range.intersects(selection)) {
            continue;
        }
        if(!main.fix.empty()) {
            out.push_back(CodeAction{
                .title = fix_title(content, main),
                .kind = protocol::CodeActionKind::QuickFix,
                .edits = placed(unit, main.fix),
            });
        }
        // A note's fix is an alternative the note itself describes.
        for(const auto& note: main.notes) {
            if(!note.fix.empty()) {
                out.push_back(CodeAction{
                    .title = note.message,
                    .kind = protocol::CodeActionKind::QuickFix,
                    .edits = placed(unit, note.fix),
                });
            }
        }
    }
}

}  // namespace clice::feature::action
