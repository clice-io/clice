#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RawCommentList.h"

namespace clice::feature::action {

namespace {

/// One definition's lines in the main file: the comment block directly
/// above it through the end of its last line.
struct Slot {
    LocalSourceRange range;
    /// Its position in declaration order.
    std::uint32_t rank;
    /// The namespace block it is written in: definitions only move
    /// within their block, whose scope their names are spelled for.
    const clang::DeclContext* block;
};

/// The lines the definition owns; nullopt when it spans a macro, another
/// file or a conditional directive, whose text cannot be moved as a
/// block.
std::optional<LocalSourceRange> definition_lines(CompilationUnitRef unit,
                                                 const clang::FunctionDecl* definition) {
    auto range = main_range(unit, written_declaration(definition)->getSourceRange());
    if(!range) {
        return std::nullopt;
    }
    auto content = unit.main_content();
    auto main = unit.main_file();
    auto begin = range->begin;

    // Comments directly above the definition travel with it: each one
    // separated from what follows by nothing but whitespace holding a
    // single line break.
    if(const auto* comments = unit.context().Comments.getCommentsInFile(main)) {
        auto it = comments->lower_bound(begin);
        while(it != comments->begin()) {
            --it;
            auto comment_end = unit.file_offset(it->second->getEndLoc()) +
                               unit.token_length(it->second->getEndLoc());
            auto gap = content.substr(comment_end, begin - comment_end);
            if(comment_end > begin || !gap.trim().empty() || gap.count('\n') != 1) {
                break;
            }
            begin = it->first;
        }
    }
    auto line = line_begin(content, begin);
    if(content.substr(line, begin - line).trim().empty()) {
        begin = line;
    }
    LocalSourceRange lines{begin, line_end(content, range->end - 1)};

    if(auto it = unit.directives().find(main); it != unit.directives().end()) {
        for(const auto& condition: it->second.conditions) {
            if(condition.loc.isValid() && unit.file_id(condition.loc) == main &&
               lines.contains(unit.file_offset(condition.loc))) {
                return std::nullopt;
            }
        }
    }
    return lines;
}

/// The edits permuting the slots of each block into rank order; empty
/// when they already are.
std::vector<TextReplacement> permutation(llvm::StringRef content, std::vector<Slot> slots) {
    std::ranges::sort(slots, {}, [](const Slot& slot) { return slot.range.begin; });
    llvm::MapVector<const clang::DeclContext*, std::vector<Slot>> blocks;
    for(auto& slot: slots) {
        blocks[slot.block].push_back(slot);
    }
    std::vector<TextReplacement> edits;
    for(auto& [block, members]: blocks) {
        auto ordered = members;
        std::ranges::sort(ordered, {}, &Slot::rank);
        for(auto [slot, target]: llvm::zip(members, ordered)) {
            if(slot.range != target.range) {
                edits.push_back(
                    {slot.range, content.substr(target.range.begin, target.range.length()).str()});
            }
        }
    }
    return edits;
}

}  // namespace

void reorder_definitions(const Context& ctx, std::vector<CodeAction>& out) {
    auto unit = ctx.unit;
    auto content = unit.main_content();
    const auto* record = ctx.node.get<clang::CXXRecordDecl>();
    const clang::FunctionDecl* anchor = nullptr;
    if(!record) {
        anchor = ctx.node.get<clang::FunctionDecl>();
        if(!anchor || !anchor->isThisDeclarationADefinition() ||
           !anchor->getLexicalDeclContext()->isFileContext()) {
            return;
        }
        if(auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(anchor)) {
            if(!method->isOutOfLine()) {
                return;
            }
            record = method->getParent();
        }
    }

    std::vector<Slot> slots;
    std::string title;
    if(record) {
        record = record->getDefinition();
        if(!record) {
            return;
        }
        llvm::DenseMap<const clang::Decl*, std::uint32_t> ranks;
        for(const auto* member: record->decls()) {
            if(auto* described = llvm::dyn_cast<clang::FunctionTemplateDecl>(member)) {
                member = described->getTemplatedDecl();
            }
            if(llvm::isa<clang::FunctionDecl>(member)) {
                ranks.try_emplace(member->getCanonicalDecl(), ranks.size());
            }
        }
        for(const auto* definition: out_of_line_definitions(unit, record)) {
            auto rank = ranks.find(definition->getCanonicalDecl());
            if(rank == ranks.end()) {
                continue;
            }
            auto lines = definition_lines(unit, definition);
            if(!lines) {
                return;
            }
            slots.push_back({*lines, rank->second, definition->getLexicalDeclContext()});
        }
        title = std::format("Reorder definitions of '{}' by declaration order", record->getName());
    } else {
        // Free functions: the definitions in this file of functions
        // declared in the file the anchor's first declaration lives in,
        // ordered as that file declares them.
        const auto* first = anchor->getCanonicalDecl();
        if(first == anchor || !first->getLocation().isFileID()) {
            return;
        }
        auto declaring = unit.file_id(first->getLocation());
        for_each_file_scope_decl(unit, [&](const clang::Decl* decl) {
            if(auto* described = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
                decl = described->getTemplatedDecl();
            }
            auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl);
            if(!function || !function->isThisDeclarationADefinition() ||
               llvm::isa<clang::CXXMethodDecl>(function)) {
                return;
            }
            const auto* canonical = function->getCanonicalDecl();
            if(canonical == function || !canonical->getLocation().isFileID() ||
               unit.file_id(canonical->getLocation()) != declaring) {
                return;
            }
            if(auto lines = definition_lines(unit, function)) {
                slots.push_back({*lines,
                                 unit.file_offset(canonical->getLocation()),
                                 function->getLexicalDeclContext()});
            }
        });
        title = "Reorder definitions by declaration order";
    }

    if(slots.size() < 2) {
        return;
    }
    auto edits = permutation(content, std::move(slots));
    if(edits.empty()) {
        return;
    }
    out.push_back(CodeAction{
        .id = "reorder-definitions",
        .title = std::move(title),
        .kind = CodeActionKind::RefactorRewrite,
        .edits = std::move(edits),
    });
}

}  // namespace clice::feature::action
