#include "Feature/Hover.h"

#include "AST/Selection.h"
#include "AST/Semantic.h"
#include "AST/Utility.h"
#include "Compiler/CompilationUnit.h"
#include "Index/Shared.h"
#include "Support/Compare.h"
#include "Support/Ranges.h"

namespace clice::feature {

namespace {

std::vector<HoverItem> getHoverItems(CompilationUnit& unit, const clang::NamedDecl* decl) {
    clang::ASTContext& Ctx = unit.context();
    std::vector<HoverItem> items;

    auto addItem = [&items](HoverItem::HoverKind kind, uint32_t value) {
        items.emplace_back(kind, llvm::Twine(value).str());
    };

    /// FIXME: Add other hover items.
    if(auto FD = llvm::dyn_cast<clang::FieldDecl>(decl)) {
        addItem(HoverItem::FieldIndex, FD->getFieldIndex());
        addItem(HoverItem::Offset, Ctx.getFieldOffset(FD));
        addItem(HoverItem::Size, Ctx.getTypeSizeInChars(FD->getType()).getQuantity());
        addItem(HoverItem::Align, Ctx.getTypeAlignInChars(FD->getType()).getQuantity());
        if(FD->isBitField()) {
            /// FIXME:
            /// addItem(HoverItem::BitWidth, FD->getBitWidthValue());
        }
    }

    return items;
}

std::string getDocument(CompilationUnit& unit, const clang::NamedDecl* decl) {
    clang::ASTContext& Ctx = unit.context();
    const clang::RawComment* comment = Ctx.getRawCommentForAnyRedecl(decl);
    if(!comment) {
        return "";
    }

    return comment->getRawText(Ctx.getSourceManager()).str();
}

std::string getQualifier(CompilationUnit& unit, const clang::NamedDecl* decl) {
    std::string result;
    llvm::raw_string_ostream os(result);
    decl->printNestedNameSpecifier(os);
    return result;
}

std::string getSourceCode(CompilationUnit& unit, const clang::NamedDecl* decl) {
    clang::SourceRange range = decl->getSourceRange();
    // auto& TB = unit.tokBuf();
    // auto& SM = unit.srcMgr();
    // auto tokens = TB.expandedTokens(range);
    /// FIXME: How to cut off the tokens?
    return "";
}

}  // namespace

Hover hover(CompilationUnit& unit, const clang::NamedDecl* decl) {
    return Hover{
        .kind = SymbolKind::from(decl),
        .name = ast::name_of(decl),
        .items = getHoverItems(unit, decl),
        .document = getDocument(unit, decl),
        .qualifier = getQualifier(unit, decl),
        .source = getSourceCode(unit, decl),
    };
}

Hover hover(CompilationUnit& unit, std::uint32_t offset) {
    Hover info;

    auto tree = SelectionTree::create_right(unit, {offset, offset});
    if(auto node = tree.common_ancestor()) {
        if(auto decl = node->get<clang::NamedDecl>()) {
            return hover(unit, decl);
        } else if(auto ref = node->get<clang::DeclRefExpr>()) {
            return hover(unit, ref->getDecl());
        }

        /// TODO: add ....
    }

    return Hover{};
}

}  // namespace clice::feature
