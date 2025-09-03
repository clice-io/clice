#include "AST/Selection.h"
#include "AST/Semantic.h"
#include "AST/Utility.h"
#include "Compiler/CompilationUnit.h"
#include "Index/Shared.h"
#include "Support/Compare.h"
#include "Support/Ranges.h"
#include "Feature/Hover.h"
#include "Support/Logger.h"

namespace clice::feature {

namespace {

std::vector<HoverItem> get_hover_items(CompilationUnit& unit,
                                       const clang::NamedDecl* decl,
                                       const config::HoverOptions& opt) {
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

std::vector<HoverItem> get_hover_items(CompilationUnit& unit,
                                       const clang::TypeLoc* typeloc,
                                       const config::HoverOptions& opt) {
    return {};
}

std::string getDocument(CompilationUnit& unit,
                        const clang::NamedDecl* decl,
                        config::HoverOptions opt) {
    clang::ASTContext& Ctx = unit.context();
    const clang::RawComment* comment = Ctx.getRawCommentForAnyRedecl(decl);
    if(!comment) {
        return "";
    }

    return comment->getRawText(Ctx.getSourceManager()).str();
}

std::string getQualifier(CompilationUnit& unit,
                         const clang::NamedDecl* decl,
                         config::HoverOptions opt) {
    std::string result;
    llvm::raw_string_ostream os(result);
    decl->printNestedNameSpecifier(os);
    return result;
}

std::string getSourceCode(CompilationUnit& unit,
                          const clang::NamedDecl* decl,
                          config::HoverOptions opt) {
    clang::SourceRange range = decl->getSourceRange();
    // auto& TB = unit.tokBuf();
    auto& sm = unit.context().getSourceManager();
    // auto tokens = TB.expandedTokens(range);
    /// FIXME: How to cut off the tokens?
    return "";
}

}  // namespace

static std::optional<clang::SourceLocation> src_loc_in_main_file(clang::SourceManager& sm,
                                                                 uint32_t off) {
    return sm.getLocForStartOfFile(sm.getMainFileID()).getLocWithOffset(off);
}

static std::optional<Hover> hover(CompilationUnit& unit,
                                  const clang::NamedDecl* decl,
                                  const config::HoverOptions& opt) {
    return Hover{
        .kind = SymbolKind::from(decl),
        .name = ast::name_of(decl),
        .items = get_hover_items(unit, decl, opt),
        .document = getDocument(unit, decl, opt),
        .qualifier = getQualifier(unit, decl, opt),
        .source = getSourceCode(unit, decl, opt),
    };
}

static std::optional<Hover> hover(CompilationUnit& unit,
                                  const clang::TypeLoc* typeloc,
                                  const config::HoverOptions& opt) {
    // TODO: Hover for type
    clice::log::warn("Hit a typeloc");
    return std::nullopt;
}

std::optional<Hover> hover(CompilationUnit& unit,
                           std::uint32_t offset,
                           const config::HoverOptions& opt) {
    auto& sm = unit.context().getSourceManager();
    auto loc = src_loc_in_main_file(sm, offset);
    if(!loc.has_value()) {
        return std::nullopt;
    }

    auto tokens_under_cursor = unit.spelled_tokens_touch(*loc);
    for(auto& tk: tokens_under_cursor) {
        clice::log::info("Hit token '{}'", tk.str());
    }

    // Find the token under cursor

    // TODO: Hover include and macros
    // TODO: Range of highlighted tokens
    // TODO: Handle `auto` and `decltype`

    auto tree = SelectionTree::create_right(unit, {offset, offset});
    if(auto node = tree.common_ancestor()) {
        if(auto decl = node->get<clang::NamedDecl>()) {
            return hover(unit, decl, opt);
        } else if(auto ref = node->get<clang::DeclRefExpr>()) {
            return hover(unit, ref->getDecl(), opt);
        } else if(auto typeloc = node->get<clang::TypeLoc>()) {
            return hover(unit, typeloc, opt);
        }

        clice::log::warn("Not selected");

        node->data.dump(llvm::errs(), unit.context());

        /// FIXME: ...
        /// - param var pointed at var decl, no param info

        /// TODO: add ....
        /// not captured:
        /// - TypeLocs
        /// - Fields inside template
        /// - NestedNameSpecifierLoc
        /// - Template specification
    } else {
        clice::log::warn("Not an ast node");
    }

    return std::nullopt;
}

}  // namespace clice::feature
