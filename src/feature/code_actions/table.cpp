module;

#include "support/prelude.h"

module clice;

import :command.file_kind;
import :compile.compilation;
import :feature.code_actions;
import :feature.code_actions.action;
import :feature.formatting;
import :semantic.selection;

namespace clice::feature {

namespace action {

namespace {

constexpr std::array specs = {
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::FunctionDecl>(),
         .reach = Reach::Ancestors,
         .run = define,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::CXXRecordDecl>(),
         .reach = Reach::Innermost,
         .run = define_missing,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::FunctionDecl>(),
         .reach = Reach::Ancestors,
         .run = define_missing,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::CXXRecordDecl>(),
         .reach = Reach::Innermost,
         .run = implement_pure_virtuals,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::CXXRecordDecl>(),
         .reach = Reach::Innermost,
         .run = memberwise_constructor,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::CXXRecordDecl>(),
         .reach = Reach::Innermost,
         .run = reorder_definitions,
         .format = false},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::FunctionDecl>(),
         .reach = Reach::Ancestors,
         .run = reorder_definitions,
         .format = false},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::SwitchStmt>(),
         .reach = Reach::Ancestors,
         .run = populate_switch,
         .format = true},
    Spec{.anchor = clang::ASTNodeKind::getFromNodeKind<clang::TypeLoc>(),
         .reach = Reach::Innermost,
         .run = expand_deduced_type,
         .format = false},
};

void format_actions(CompilationUnit& unit, std::vector<CodeAction>& actions, std::size_t from) {
    auto path = unit.file_path(unit.focus_file().file());
    for(auto& action: views::drop(actions, from)) {
        if(!action.edits.empty()) {
            action.edits = format_edits(path, unit.focus_file().content(), std::move(action.edits));
        }
    }
}

/// Walk the ancestor chain of the selection's innermost node and offer
/// each spec its innermost matching node, once.
void enumerate(CompilationUnit& unit,
               const SelectionTree& tree,
               bool focus_is_header,
               std::vector<CodeAction>& out) {
    const auto* innermost = tree.common_ancestor();
    if(!innermost) {
        return;
    }

    std::bitset<specs.size()> fired;
    for(const auto* node = innermost; node; node = node->parent) {
        auto kind = node->data.getNodeKind();
        for(auto [index, spec]: views::enumerate(specs)) {
            if(fired[index] || (spec.reach == Reach::Innermost && node != innermost) ||
               !spec.anchor.isBaseOf(kind)) {
                continue;
            }
            fired[index] = true;
            auto before = out.size();
            spec.run(Context{unit, *node, focus_is_header}, out);
            if(spec.format) {
                format_actions(unit, out, before);
            }
        }
    }
}

}  // namespace

}  // namespace action

auto code_actions(CompilationUnit& unit, LocalSourceRange selection) -> std::vector<CodeAction> {
    std::vector<CodeAction> out;
    action::fix_its(unit, selection, out);
    action::add_include(unit, selection, out);

    auto path = unit.file_path(unit.focus_file().file());
    bool focus_is_header = is_header_path(path);
    SelectionTree::create_each(unit, selection, [&](SelectionTree tree) {
        auto before = out.size();
        action::enumerate(unit, tree, focus_is_header, out);
        return out.size() > before;
    });

    auto before = out.size();
    action::expand_macro(unit, selection, out);
    action::format_actions(unit, out, before);
    assert(ranges::all_of(out, [](const CodeAction& action) {
        return ranges::any_of(code_action_kinds,
                              [&](llvm::StringRef kind) { return kind == action.kind; });
    }));
    return out;
}

}  // namespace clice::feature
