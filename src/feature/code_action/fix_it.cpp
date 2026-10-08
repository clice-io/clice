module;

#include "modules/prelude.h"

module clice;

import :compile.compilation_unit;
import :feature.code_action.action;

namespace clice::feature::action {

namespace {

/// Code as a fix's title shows it: its first line, cut short.
std::string title_code(llvm::StringRef code) {
    if(code == "\n") {
        return R"(\n)";
    }
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

}  // namespace

void fix_its(CompilationUnitRef unit, LocalSourceRange selection, std::vector<CodeAction>& out) {
    auto content = unit.main_content();
    for_each_diagnostic(unit, [&](const Diagnostic& main, llvm::ArrayRef<Diagnostic> notes) {
        if(main.fid != unit.main_file() || !main.range.intersects(selection)) {
            return;
        }
        if(!main.fix.empty()) {
            out.push_back(CodeAction{
                .title = fix_title(content, main),
                .kind = protocol::CodeActionKind::QuickFix,
                .edits = main.fix,
            });
        }
        // A note's fix is an alternative the note itself describes.
        for(const auto& note: notes) {
            if(!note.fix.empty()) {
                out.push_back(CodeAction{
                    .title = note.message,
                    .kind = protocol::CodeActionKind::QuickFix,
                    .edits = note.fix,
                });
            }
        }
    });
}

}  // namespace clice::feature::action
