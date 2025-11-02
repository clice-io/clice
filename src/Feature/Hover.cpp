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

static std::vector<HoverItem> get_hover_items(CompilationUnitRef unit,
                                              const clang::NamedDecl* decl,
                                              const config::HoverOptions& opt) {
    clang::ASTContext& ctx = unit.context();
    std::vector<HoverItem> items;

    auto add_item = [&items](HoverItem::HoverKind kind, std::string&& val) {
        items.emplace_back(kind, val);
    };

    /// TODO: Add other hover items.
    if(auto fd = llvm::dyn_cast<clang::FieldDecl>(decl)) {
        clice::logging::warn("Got a field decl");
        const auto record = fd->getParent();
        add_item(HoverItem::Type, fd->getType().getAsString());

        /// Remove in release mode
        /// add_item(HoverItem::FieldIndex, llvm::Twine(fd->getFieldIndex()).str());
        if(!record->isDependentType()) {
            add_item(HoverItem::Offset, llvm::Twine(ctx.getFieldOffset(fd)).str());
            add_item(HoverItem::Align,
                     llvm::Twine(ctx.getTypeAlignInChars(fd->getType()).getQuantity()).str());
            add_item(HoverItem::Size,
                     llvm::Twine(ctx.getTypeSizeInChars(fd->getType()).getQuantity()).str());
        }

        if(record->isUnion()) {
            add_item(HoverItem::Size,
                     llvm::Twine(ctx.getTypeSizeInChars(fd->getType()).getQuantity()).str());
            add_item(HoverItem::Align,
                     llvm::Twine(ctx.getTypeAlignInChars(fd->getType()).getQuantity()).str());
        }

        if(fd->isBitField()) {
            add_item(HoverItem::BitWidth, llvm::Twine(fd->getBitWidthValue()).str());
            clice::logging::warn("Got bit field, name: {}, bitwidth: {}",
                                 fd->getName(),
                                 fd->getBitWidthValue());
        }
    } else if(auto vd = llvm::dyn_cast<clang::VarDecl>(decl)) {
        clice::logging::warn("Got a var decl");
        add_item(HoverItem::Type, vd->getType().getAsString());
    }

    return items;
}

static std::vector<HoverItem> get_hover_items(CompilationUnit& unit,
                                              const clang::TypeLoc* typeloc,
                                              const config::HoverOptions& opt) {
    return {};
}

static std::string get_document(CompilationUnitRef unit,
                                const clang::NamedDecl* decl,
                                config::HoverOptions opt) {
    clang::ASTContext& Ctx = unit.context();
    const clang::RawComment* comment = Ctx.getRawCommentForAnyRedecl(decl);
    if(!comment) {
        return "";
    }
    auto raw_string = comment->getRawText(Ctx.getSourceManager()).str();
    return "";
}

static std::string get_qualifier(CompilationUnitRef unit,
                                 const clang::NamedDecl* decl,
                                 config::HoverOptions opt) {
    std::string result;
    llvm::raw_string_ostream os(result);
    decl->printNestedNameSpecifier(os);
    return result;
}

// Get all source code
static std::string get_source_code(CompilationUnitRef unit, clang::SourceRange range) {
    clang::LangOptions lo;
    auto& sm = unit.context().getSourceManager();
    auto start_loc = sm.getSpellingLoc(range.getBegin());
    auto last_token_loc = sm.getSpellingLoc(range.getEnd());
    auto end_loc = clang::Lexer::getLocForEndOfToken(last_token_loc, 0, sm, lo);
    return std::string{clang::Lexer::getSourceText(
        clang::CharSourceRange::getCharRange(clang::SourceRange{start_loc, end_loc}),
        sm,
        lo)};
}

// TODO: How does clangd put together decl, name, scope and sometimes initialized value?
// ```
// // scope
// <Access specifier> <type> <name> <initialized value>
// ```
static std::string get_source_code(CompilationUnit& unit,
                                   const clang::NamedDecl* decl,
                                   config::HoverOptions opt) {
    clang::SourceRange range = decl->getSourceRange();
    return get_source_code(unit, range);
}

static std::optional<Hover> hover(CompilationUnitRef unit,
                                  const clang::NamedDecl* decl,
                                  const config::HoverOptions& opt) {
    return Hover{
        .kind = SymbolKind::from(decl),
        .name = ast::name_of(decl),
        .items = get_hover_items(unit, decl, opt),
        .document = get_document(unit, decl, opt),
        .qualifier = get_qualifier(unit, decl, opt),
        .source = get_source_code(unit, decl, opt),
    };
}

static std::optional<Hover> hover(CompilationUnitRef unit,
                                  const clang::TypeLoc* typeloc,
                                  const config::HoverOptions& opt) {
    // TODO: Hover for type
    clice::logging::warn("Hit a typeloc");
    typeloc->dump(llvm::errs(), unit.context());
    auto ty = typeloc->getType();
    return Hover{.kind = SymbolKind::Type, .name = ty.getAsString()};
}

static std::optional<Hover> hover(CompilationUnit& unit,
                                  const SelectionTree::Node* node,
                                  const config::HoverOptions& opt) {
    using namespace clang;
    auto Kind = node->data.getNodeKind();
    clice::logging::warn("Node kind is: {}", Kind.asStringRef());

#define kind_flag_def(Ty) static constexpr auto Flag##Ty = ASTNodeKind::getFromNodeKind<Ty>()
    kind_flag_def(QualType);
    kind_flag_def(TypeLoc);
    kind_flag_def(Decl);
    kind_flag_def(Stmt);
    kind_flag_def(Type);
    kind_flag_def(OMPClause);
    kind_flag_def(TemplateArgument);
    kind_flag_def(TemplateArgumentLoc);
    kind_flag_def(LambdaCapture);
    kind_flag_def(TemplateName);
    kind_flag_def(NestedNameSpecifierLoc);
    kind_flag_def(Attr);
    kind_flag_def(ObjCProtocolLoc);

#define is_in_range(LHS, RHS)                                                                      \
    (!((Kind < Flag##LHS) && (Kind.isSame(Flag##LHS))) && (Kind < Flag##RHS))

#define is(flag) (Kind.isSame(Flag##flag))

    if(is(NestedNameSpecifierLoc)) {
        clice::logging::warn("Hit a `NestedNameSpecifierLoc`");
    } else if(is_in_range(QualType, TypeLoc)) {
        // Typeloc
        clice::logging::warn("Hit a `TypeLoc`");
        if(auto typeloc = node->get<clang::TypeLoc>()) {
            return hover(unit, typeloc, opt);
        }
    } else if(is_in_range(Decl, Stmt)) {
        // Decl
        clice::logging::warn("Hit a `Decl`");
        if(auto decl = node->get<clang::NamedDecl>()) {
            return hover(unit, decl, opt);
        } else {
            clice::logging::warn("Not intersted");
        }
    } else if(is_in_range(Attr, ObjCProtocolLoc)) {
        clice::logging::warn("Hit an `Attr`");
        // TODO: Attr
    } else {
        // Not interested
        clice::logging::warn("Not interested");
    }

#undef is
#undef is_in_range
#undef kind_flag_def

    return std::nullopt;
}

}  // namespace

std::optional<Hover> hover(CompilationUnitRef unit,
                           std::uint32_t offset,
                           const config::HoverOptions& opt) {
    auto& sm = unit.context().getSourceManager();

    auto src_loc_in_main_file = [&sm](uint32_t off) -> std::optional<clang::SourceLocation> {
        return sm.getLocForStartOfFile(sm.getMainFileID()).getLocWithOffset(off);
    };

    auto loc = src_loc_in_main_file(offset);
    if(!loc.has_value()) {
        clice::logging::warn("Invalid file offset, cannot get location");
        return std::nullopt;
    }

    // Handle includsions
    bool linenr_invalid = false;
    unsigned linenr = sm.getPresumedLineNumber(*loc, &linenr_invalid);
    if(linenr_invalid) {
        clice::logging::warn("Invalid location, cannot get linenr");
        return std::nullopt;
    }
    clice::logging::warn("Hover at linenr: {}", linenr);
    auto directive = unit.directives()[sm.getMainFileID()];
    for(auto& inclusion: directive.includes) {
        bool invalid = false;
        auto inc_linenr = sm.getPresumedLineNumber(inclusion.location, &invalid);
        if(!invalid && inc_linenr == linenr) {
            auto raw_name = get_source_code(unit, inclusion.filename_range);
            auto file_name = llvm::StringRef{raw_name}.trim("<>\"");
            Hover hi;
            hi.kind = SymbolKind::Directive;
            hi.name = file_name;
            auto dir = sm.getFileEntryForID(inclusion.fid)->tryGetRealPathName();
            hi.source = dir;
            // TODO: Provides symbol
            return hi;
        }
    }

    auto tree = SelectionTree::create_right(unit, {offset, offset});
    if(auto node = tree.common_ancestor()) {
        return hover(unit, node, opt);
    } else {
        clice::logging::warn("Not an ast node");
    }

    return std::nullopt;
}

std::optional<std::string> Hover::get_item_content(HoverItem::HoverKind kind) {
    for(auto& item: this->items) {
        if(item.kind == kind) {
            return item.value;
        }
    }
    return std::nullopt;
}

std::optional<std::string> Hover::display(config::HoverOptions opt) {
    std::string content;
    llvm::raw_string_ostream os(content);
    os << std::format("{}: {}\n", this->kind.name(), this->name);
    os << std::format("Contains {} items\n", this->items.size());
    for(auto& hi: this->items) {
        os << std::format("- {}: {}\n", clice::refl::enum_name(hi.kind), hi.value);
    }
    os << "---\n";
    os << "Document:\n```text\n" << this->document << "\n```\n";
    os << "---\n";
    os << "Source code:\n```cpp\n" << this->source << "\n```\n";
    return os.str();
}

}  // namespace clice::feature
