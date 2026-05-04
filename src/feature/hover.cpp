// Hover information for C++ symbols.
// Logic adapted from clangd's Hover.cpp (llvm-project/clang-tools-extra/clangd/Hover.cpp).

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "semantic/ast_utility.h"
#include "semantic/selection.h"
#include "semantic/symbol_kind.h"
#include "support/doxygen.h"
#include "support/markup.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Type.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Lex/Lexer.h"

namespace clice::feature {

namespace {

struct PrintedType {
    std::string type;
    std::optional<std::string> aka;
};

struct Param {
    std::optional<PrintedType> type;
    std::optional<std::string> name;
    std::optional<std::string> default_value;
};

struct HoverInfo {
    std::optional<std::string> namespace_scope;
    std::string local_scope;
    std::string name;
    SymbolKind kind = SymbolKind::Invalid;
    std::string documentation;
    std::string definition;
    std::string access_specifier;
    std::optional<PrintedType> type;
    std::optional<PrintedType> return_type;
    std::optional<std::vector<Param>> parameters;
    std::optional<std::vector<Param>> template_parameters;
    std::optional<std::string> value;
    std::optional<std::uint64_t> size;
    std::optional<std::uint64_t> offset;
    std::optional<std::uint64_t> padding;
    std::optional<std::uint64_t> align;
};

auto symbol_name(SymbolKind kind) -> llvm::StringRef {
    switch(kind) {
        case SymbolKind::Module: return "module";
        case SymbolKind::Namespace: return "namespace";
        case SymbolKind::Class: return "class";
        case SymbolKind::Struct: return "struct";
        case SymbolKind::Union: return "union";
        case SymbolKind::Enum: return "enum";
        case SymbolKind::Type: return "type";
        case SymbolKind::Concept: return "concept";
        case SymbolKind::Field: return "field";
        case SymbolKind::EnumMember: return "enum member";
        case SymbolKind::Function: return "function";
        case SymbolKind::Method: return "method";
        case SymbolKind::Variable: return "variable";
        case SymbolKind::Parameter: return "parameter";
        case SymbolKind::Macro: return "macro";
        default: return "symbol";
    }
}

auto get_printing_policy(clang::ASTContext& ctx) -> clang::PrintingPolicy {
    clang::PrintingPolicy pp(ctx.getPrintingPolicy());
    pp.SuppressScope = true;
    pp.AnonymousTagLocations = false;
    pp.PolishForDeclaration = true;
    pp.ConstantsAsWritten = true;
    pp.SuppressTemplateArgsInCXXConstructors = true;
    pp.SuppressDefaultTemplateArgs = true;
    return pp;
}

auto get_namespace_scope(const clang::Decl* D) -> std::string {
    const clang::DeclContext* DC = D->getDeclContext();

    if(const auto* TD = llvm::dyn_cast<clang::TagDecl>(DC))
        return get_namespace_scope(TD);
    if(const auto* FD = llvm::dyn_cast<clang::FunctionDecl>(DC))
        return get_namespace_scope(FD);
    if(const auto* NSD = llvm::dyn_cast<clang::NamespaceDecl>(DC)) {
        if(NSD->isInline() || NSD->isAnonymousNamespace())
            return get_namespace_scope(NSD);
    }
    if(const auto* ND = llvm::dyn_cast<clang::NamedDecl>(DC)) {
        std::string result;
        llvm::raw_string_ostream os(result);
        ND->printQualifiedName(os);
        return result;
    }

    return "";
}

auto get_local_scope(const clang::Decl* D) -> std::string {
    std::vector<std::string> scopes;
    const clang::DeclContext* DC = D->getDeclContext();

    auto get_name = [](const clang::TypeDecl* TD) -> std::string {
        if(!TD->getDeclName().isEmpty()) {
            clang::PrintingPolicy policy = TD->getASTContext().getPrintingPolicy();
            policy.SuppressScope = true;
            return clang::QualType(TD->getTypeForDecl(), 0).getAsString(policy);
        }
        if(auto* RD = llvm::dyn_cast<clang::RecordDecl>(TD))
            return std::format("(anonymous {})", RD->getKindName().str());
        return "";
    };

    while(DC) {
        if(const auto* TD = llvm::dyn_cast<clang::TypeDecl>(DC))
            scopes.push_back(get_name(TD));
        else if(const auto* FD = llvm::dyn_cast<clang::FunctionDecl>(DC))
            scopes.push_back(FD->getNameAsString());
        DC = DC->getParent();
    }

    std::string result;
    for(auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        if(!it->empty()) {
            result += *it;
            result += "::";
        }
    }
    return result;
}

auto print_definition(const clang::Decl* D, clang::PrintingPolicy pp, CompilationUnitRef unit)
    -> std::string {
    if(auto* VD = llvm::dyn_cast<clang::VarDecl>(D)) {
        if(auto* IE = VD->getInit()) {
            if(200 < unit.expanded_tokens(IE->getSourceRange()).size())
                pp.SuppressInitializers = true;
        }
    }
    std::string definition;
    llvm::raw_string_ostream os(definition);
    D->print(os, pp);
    return definition;
}

auto print_type(clang::QualType QT,
                clang::ASTContext& ctx,
                const clang::PrintingPolicy& pp,
                bool show_aka) -> PrintedType {
    while(!QT.isNull() && QT->isDecltypeType())
        QT = QT->castAs<clang::DecltypeType>()->getUnderlyingType();

    PrintedType result;
    llvm::raw_string_ostream os(result.type);

    if(!QT.isNull() && !QT.hasQualifiers() && pp.SuppressTagKeyword) {
        if(auto* TT = llvm::dyn_cast<clang::TagType>(QT.getTypePtr()))
            os << TT->getDecl()->getKindName() << " ";
    }
    QT.print(os, pp);

    if(!QT.isNull() && show_aka) {
        bool should_aka = false;
        clang::QualType desugared = clang::desugarForDiagnostic(ctx, QT, should_aka);
        if(should_aka)
            result.aka = desugared.getAsString(pp);
    }

    return result;
}

auto print_type(const clang::TemplateTypeParmDecl* TTP) -> PrintedType {
    PrintedType result;
    result.type = TTP->wasDeclaredWithTypename() ? "typename" : "class";
    if(TTP->isParameterPack())
        result.type += "...";
    return result;
}

auto print_type(const clang::NonTypeTemplateParmDecl* NTTP,
                const clang::PrintingPolicy& pp,
                bool show_aka) -> PrintedType {
    auto result = print_type(NTTP->getType(), NTTP->getASTContext(), pp, show_aka);
    if(NTTP->isParameterPack()) {
        result.type += "...";
        if(result.aka)
            *result.aka += "...";
    }
    return result;
}

auto print_type(const clang::TemplateTemplateParmDecl* TTP,
                const clang::PrintingPolicy& pp,
                bool show_aka) -> PrintedType {
    PrintedType result;
    llvm::raw_string_ostream os(result.type);
    os << "template <";
    llvm::StringRef sep = "";
    for(const clang::Decl* param: *TTP->getTemplateParameters()) {
        os << sep;
        sep = ", ";
        if(const auto* ttp = llvm::dyn_cast<clang::TemplateTypeParmDecl>(param))
            os << print_type(ttp).type;
        else if(const auto* nttp = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(param))
            os << print_type(nttp, pp, show_aka).type;
        else if(const auto* ttpd = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(param))
            os << print_type(ttpd, pp, show_aka).type;
    }
    os << "> class";
    return result;
}

auto fetch_template_parameters(const clang::TemplateParameterList* params,
                               const clang::PrintingPolicy& pp,
                               bool show_aka) -> std::vector<Param> {
    std::vector<Param> result;

    for(const clang::Decl* param: *params) {
        Param p;
        if(const auto* TTP = llvm::dyn_cast<clang::TemplateTypeParmDecl>(param)) {
            p.type = print_type(TTP);
            if(!TTP->getName().empty())
                p.name = TTP->getNameAsString();
            if(TTP->hasDefaultArgument()) {
                p.default_value.emplace();
                llvm::raw_string_ostream os(*p.default_value);
                TTP->getDefaultArgument().getArgument().print(pp, os, false);
            }
        } else if(const auto* NTTP = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(param)) {
            p.type = print_type(NTTP, pp, show_aka);
            if(clang::IdentifierInfo* II = NTTP->getIdentifier())
                p.name = II->getName().str();
            if(NTTP->hasDefaultArgument()) {
                p.default_value.emplace();
                llvm::raw_string_ostream os(*p.default_value);
                NTTP->getDefaultArgument().getArgument().print(pp, os, false);
            }
        } else if(const auto* TTPD = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(param)) {
            p.type = print_type(TTPD, pp, show_aka);
            if(!TTPD->getName().empty())
                p.name = TTPD->getNameAsString();
            if(TTPD->hasDefaultArgument()) {
                p.default_value.emplace();
                llvm::raw_string_ostream os(*p.default_value);
                TTPD->getDefaultArgument().getArgument().print(pp, os, false);
            }
        }
        result.push_back(std::move(p));
    }

    return result;
}

auto get_default_arg(const clang::ParmVarDecl* PVD) -> const clang::Expr* {
    if(!PVD->hasDefaultArg() || PVD->hasUnparsedDefaultArg())
        return nullptr;
    return PVD->hasUninstantiatedDefaultArg() ? PVD->getUninstantiatedDefaultArg()
                                              : PVD->getDefaultArg();
}

auto to_param(const clang::ParmVarDecl* PVD, const clang::PrintingPolicy& pp, bool show_aka)
    -> Param {
    Param out;
    out.type = print_type(PVD->getType(), PVD->getASTContext(), pp, show_aka);
    if(!PVD->getName().empty())
        out.name = PVD->getNameAsString();
    if(const clang::Expr* def_arg = get_default_arg(PVD)) {
        out.default_value.emplace();
        llvm::raw_string_ostream os(*out.default_value);
        def_arg->printPretty(os, nullptr, pp);
    }
    return out;
}

auto get_underlying_function(const clang::Decl* D) -> const clang::FunctionDecl* {
    if(const auto* VD = llvm::dyn_cast<clang::VarDecl>(D)) {
        auto QT = VD->getType();
        if(!QT.isNull()) {
            while(!QT->getPointeeType().isNull())
                QT = QT->getPointeeType();
            if(const auto* CD = QT->getAsCXXRecordDecl())
                return CD->getLambdaCallOperator();
        }
    }
    return D->getAsFunction();
}

void fill_function_type_and_params(HoverInfo& hi,
                                   const clang::Decl* D,
                                   const clang::FunctionDecl* FD,
                                   const clang::PrintingPolicy& pp,
                                   bool show_aka) {
    hi.parameters.emplace();
    for(const clang::ParmVarDecl* PVD: FD->parameters())
        hi.parameters->emplace_back(to_param(PVD, pp, show_aka));

    auto NK = FD->getDeclName().getNameKind();
    if(NK == clang::DeclarationName::CXXConstructorName ||
       NK == clang::DeclarationName::CXXDestructorName ||
       NK == clang::DeclarationName::CXXConversionFunctionName)
        return;

    hi.return_type = print_type(FD->getReturnType(), FD->getASTContext(), pp, show_aka);
    clang::QualType QT = FD->getType();
    if(const auto* VD = llvm::dyn_cast<clang::VarDecl>(D))
        QT = VD->getType().getDesugaredType(D->getASTContext());
    hi.type = print_type(QT, D->getASTContext(), pp, show_aka);
}

auto print_hex(const llvm::APSInt& V) -> llvm::FormattedNumber {
    uint64_t bits = V.getBitWidth() > 64 ? V.trunc(64).getZExtValue() : V.getZExtValue();
    if(V.isNegative() && V.getSignificantBits() <= 32)
        return llvm::format_hex(uint32_t(bits), 0);
    return llvm::format_hex(bits, 0);
}

auto print_expr_value(const clang::Expr* E, const clang::ASTContext& ctx)
    -> std::optional<std::string> {
    if(const auto* ILE = llvm::dyn_cast<clang::InitListExpr>(E)) {
        if(!ILE->isSemanticForm())
            E = ILE->getSemanticForm();
    }

    clang::QualType T = E->getType();
    if(T.isNull() || T->isFunctionType() || T->isFunctionPointerType() ||
       T->isFunctionReferenceType() || T->isVoidType())
        return std::nullopt;

    clang::Expr::EvalResult constant;
    if(E->isValueDependent() || !E->EvaluateAsRValue(constant, ctx) || constant.Val.isStruct() ||
       constant.Val.isUnion())
        return std::nullopt;

    if(T->isEnumeralType() && constant.Val.isInt() &&
       constant.Val.getInt().getSignificantBits() <= 64) {
        int64_t val = constant.Val.getInt().getExtValue();
        for(const clang::EnumConstantDecl* ECD:
            T->castAs<clang::EnumType>()->getDecl()->enumerators()) {
            if(ECD->getInitVal() == val) {
                std::string result;
                llvm::raw_string_ostream os(result);
                os << ECD->getNameAsString() << " (" << print_hex(constant.Val.getInt()) << ")";
                return result;
            }
        }
    }

    if(T->isIntegralOrEnumerationType() && constant.Val.isInt() &&
       constant.Val.getInt().getSignificantBits() <= 64 && constant.Val.getInt().uge(10)) {
        std::string result;
        llvm::raw_string_ostream os(result);
        os << constant.Val.getAsString(ctx, T) << " (" << print_hex(constant.Val.getInt()) << ")";
        return result;
    }

    return constant.Val.getAsString(ctx, T);
}

auto get_decl_for_comment(const clang::NamedDecl* D) -> const clang::NamedDecl* {
    const clang::NamedDecl* result = D;
    if(const auto* TSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(D)) {
        if(TSD->getTemplateSpecializationKind() == clang::TSK_Undeclared)
            result = TSD->getSpecializedTemplate();
        else if(const auto* TIP = TSD->getTemplateInstantiationPattern())
            result = TIP;
    } else if(const auto* TSD = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(D)) {
        if(TSD->getTemplateSpecializationKind() == clang::TSK_Undeclared)
            result = TSD->getSpecializedTemplate();
        else if(const auto* TIP = TSD->getTemplateInstantiationPattern())
            result = TIP;
    } else if(const auto* FD = D->getAsFunction()) {
        if(const auto* TIP = FD->getTemplateInstantiationPattern())
            result = TIP;
    }
    if(D != result)
        result = get_decl_for_comment(result);
    return result;
}

auto get_documentation(const clang::NamedDecl* D, clang::ASTContext& ctx) -> std::string {
    const clang::NamedDecl* comment_decl = get_decl_for_comment(D);
    const clang::RawComment* raw = ctx.getRawCommentForDeclNoCache(comment_decl);
    if(!raw)
        return "";
    return raw->getFormattedText(ctx.getSourceManager(), ctx.getDiagnostics());
}

void add_layout_info(const clang::NamedDecl& ND, HoverInfo& hi) {
    if(ND.isInvalidDecl())
        return;

    const auto& ctx = ND.getASTContext();

    if(auto* RD = llvm::dyn_cast<clang::RecordDecl>(&ND)) {
        if(auto size = ctx.getTypeSizeInCharsIfKnown(RD->getTypeForDecl()))
            hi.size = size->getQuantity() * 8;
        if(!RD->isDependentType() && RD->isCompleteDefinition())
            hi.align = ctx.getTypeAlign(RD->getTypeForDecl());
        return;
    }

    if(const auto* FD = llvm::dyn_cast<clang::FieldDecl>(&ND)) {
        const auto* record = FD->getParent();
        if(record)
            record = record->getDefinition();
        if(record && !record->isInvalidDecl() && !record->isDependentType()) {
            hi.align = ctx.getTypeAlign(FD->getType());
            const clang::ASTRecordLayout& layout = ctx.getASTRecordLayout(record);
            hi.offset = layout.getFieldOffset(FD->getFieldIndex());
            if(FD->isBitField())
                hi.size = FD->getBitWidthValue();
            else if(auto size = ctx.getTypeSizeInCharsIfKnown(FD->getType()))
                hi.size = FD->isZeroSize(ctx) ? 0 : size->getQuantity() * 8;
            if(hi.size) {
                unsigned end_of_field = *hi.offset + *hi.size;
                if(!record->isUnion() && FD->getFieldIndex() + 1 < layout.getFieldCount()) {
                    unsigned next_offset = layout.getFieldOffset(FD->getFieldIndex() + 1);
                    if(next_offset >= end_of_field)
                        hi.padding = next_offset - end_of_field;
                } else {
                    hi.padding = layout.getSize().getQuantity() * 8 - end_of_field;
                }
            }
            if(record->isUnion())
                hi.offset.reset();
        }
    }
}

auto hover_decl(const clang::NamedDecl* D,
                CompilationUnitRef unit,
                const clang::PrintingPolicy& pp,
                const HoverOptions& options) -> HoverInfo {
    HoverInfo hi;
    auto& ctx = D->getASTContext();

    hi.access_specifier = clang::getAccessSpelling(D->getAccess()).str();
    hi.namespace_scope = get_namespace_scope(D);
    if(!hi.namespace_scope->empty())
        hi.namespace_scope->append("::");
    hi.local_scope = get_local_scope(D);
    if(!hi.local_scope.empty())
        hi.local_scope.append("::");

    hi.name = ast::name_of(D);
    hi.documentation = get_documentation(D, ctx);
    hi.kind = SymbolKind::from(D);

    if(const clang::TemplateDecl* TD = D->getDescribedTemplate()) {
        hi.template_parameters =
            fetch_template_parameters(TD->getTemplateParameters(), pp, options.show_aka);
        D = TD;
    } else if(const clang::FunctionDecl* FD = D->getAsFunction()) {
        if(const auto* FTD = FD->getDescribedTemplate()) {
            hi.template_parameters =
                fetch_template_parameters(FTD->getTemplateParameters(), pp, options.show_aka);
            D = FTD;
        }
    }

    if(const clang::FunctionDecl* FD = get_underlying_function(D))
        fill_function_type_and_params(hi, D, FD, pp, options.show_aka);
    else if(const auto* VD = llvm::dyn_cast<clang::ValueDecl>(D))
        hi.type = print_type(VD->getType(), ctx, pp, options.show_aka);
    else if(const auto* TTP = llvm::dyn_cast<clang::TemplateTypeParmDecl>(D))
        hi.type = PrintedType{TTP->wasDeclaredWithTypename() ? "typename" : "class"};
    else if(const auto* TTP = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(D))
        hi.type = print_type(TTP, pp, options.show_aka);
    else if(const auto* VT = llvm::dyn_cast<clang::VarTemplateDecl>(D))
        hi.type = print_type(VT->getTemplatedDecl()->getType(), ctx, pp, options.show_aka);
    else if(const auto* TN = llvm::dyn_cast<clang::TypedefNameDecl>(D))
        hi.type =
            print_type(TN->getUnderlyingType().getDesugaredType(ctx), ctx, pp, options.show_aka);
    else if(const auto* TAT = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(D))
        hi.type =
            print_type(TAT->getTemplatedDecl()->getUnderlyingType(), ctx, pp, options.show_aka);

    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(D); var && !var->isInvalidDecl()) {
        if(const clang::Expr* init = var->getInit())
            hi.value = print_expr_value(init, ctx);
    } else if(const auto* ECD = llvm::dyn_cast<clang::EnumConstantDecl>(D)) {
        if(!ECD->getType()->isDependentType())
            hi.value = llvm::toString(ECD->getInitVal(), 10);
    }

    hi.definition = print_definition(D, pp, unit);
    return hi;
}

auto hover_macro(CompilationUnitRef unit, clang::SourceLocation loc) -> std::optional<HoverInfo> {
    auto fid = unit.interested_file();
    auto& directives = unit.directives();
    auto it = directives.find(fid);
    if(it == directives.end())
        return std::nullopt;

    auto offset = unit.file_offset(loc);

    const MacroRef* found = nullptr;
    for(auto& macro: it->second.macros) {
        if(macro.kind != MacroRef::Ref && macro.kind != MacroRef::Def)
            continue;
        auto [macro_fid, macro_offset] = unit.decompose_location(macro.loc);
        if(macro_fid != fid)
            continue;
        auto spelling = unit.token_spelling(macro.loc);
        if(offset >= macro_offset && offset < macro_offset + spelling.size()) {
            found = &macro;
            break;
        }
    }

    if(!found)
        return std::nullopt;

    HoverInfo hi;
    hi.name = unit.token_spelling(found->loc).str();
    hi.kind = SymbolKind::Macro;

    const clang::MacroInfo* MI = found->macro;
    if(!MI)
        return hi;

    auto& SM = unit.context().getSourceManager();
    clang::SourceLocation start_loc = MI->getDefinitionLoc();
    clang::SourceLocation end_loc = MI->getDefinitionEndLoc();

    if(SM.getPresumedLoc(end_loc, false).isValid()) {
        end_loc = clang::Lexer::getLocForEndOfToken(end_loc, 0, SM, unit.lang_options());
        bool invalid = false;
        llvm::StringRef buffer = SM.getBufferData(SM.getFileID(start_loc), &invalid);
        if(!invalid) {
            unsigned start_offset = SM.getFileOffset(start_loc);
            unsigned end_offset = SM.getFileOffset(end_loc);
            if(end_offset <= buffer.size() && start_offset < end_offset)
                hi.definition =
                    ("#define " + buffer.substr(start_offset, end_offset - start_offset)).str();
        }
    }

    return hi;
}

auto type_as_definition(const PrintedType& ptype) -> std::string {
    std::string result;
    llvm::raw_string_ostream os(result);
    os << ptype.type;
    if(ptype.aka)
        os << " // aka: " << *ptype.aka;
    return result;
}

auto hover_this_expr(const clang::CXXThisExpr* CTE,
                     clang::ASTContext& ctx,
                     const clang::PrintingPolicy& pp,
                     bool show_aka) -> HoverInfo {
    clang::QualType origin_type = CTE->getType()->getPointeeType();
    clang::QualType class_type = clang::QualType(origin_type->getAsTagDecl()->getTypeForDecl(), 0);
    clang::QualType pretty_type = ctx.getPointerType(
        clang::QualType(class_type.getTypePtr(), origin_type.getCVRQualifiers()));

    HoverInfo hi;
    hi.name = "this";
    hi.definition = type_as_definition(print_type(pretty_type, ctx, pp, show_aka));
    return hi;
}

auto hover_deduced_type(clang::QualType QT,
                        const clang::syntax::Token& tok,
                        clang::ASTContext& ctx,
                        const clang::PrintingPolicy& pp,
                        bool show_aka) -> HoverInfo {
    HoverInfo hi;
    hi.name = clang::tok::getTokenName(tok.kind());
    hi.kind = SymbolKind::Type;

    if(QT->isUndeducedAutoType()) {
        hi.definition = "/* not deduced */";
    } else {
        hi.definition = type_as_definition(print_type(QT, ctx, pp, show_aka));
        if(const auto* D = QT->getAsTagDecl()) {
            const auto* comment_decl = get_decl_for_comment(D);
            hi.documentation = get_documentation(comment_decl, ctx);
        }
    }

    return hi;
}

auto format_size(std::uint64_t size_in_bits) -> std::string {
    std::uint64_t value = size_in_bits % 8 == 0 ? size_in_bits / 8 : size_in_bits;
    const char* unit = value != 0 && value == size_in_bits ? "bit" : "byte";
    return std::format("{} {}{}", value, unit, value == 1 ? "" : "s");
}

auto format_offset(std::uint64_t offset_in_bits) -> std::string {
    auto bytes = offset_in_bits / 8;
    auto bits = offset_in_bits % 8;
    auto result = format_size(bytes * 8);
    if(bits != 0)
        result += " and " + format_size(bits);
    return result;
}

auto format_param(const Param& p) -> std::string {
    std::string result;
    if(p.type)
        result += p.type->type;
    if(p.name) {
        if(!result.empty())
            result += " ";
        result += *p.name;
    }
    if(p.default_value) {
        result += " = ";
        result += *p.default_value;
    }
    return result;
}

auto present(const HoverInfo& hi) -> std::string {
    Markup output;

    Paragraph& header = output.add_heading(3);
    if(hi.kind != SymbolKind::Invalid)
        header.append_text(symbol_name(hi.kind).str())
            .append_text(hi.name, Paragraph::Kind::InlineCode);
    else
        header.append_text(hi.name, Paragraph::Kind::InlineCode);

    output.add_ruler();

    if(hi.return_type) {
        std::string ret_str = hi.return_type->type;
        if(hi.return_type->aka)
            ret_str += " (aka " + *hi.return_type->aka + ")";
        output.add_paragraph()
            .append_text("\xe2\x86\x92")
            .append_text(ret_str, Paragraph::Kind::InlineCode);
    }

    if(hi.parameters && !hi.parameters->empty()) {
        output.add_paragraph().append_text("Parameters:");
        auto& list = output.add_bullet_list();
        for(auto& param: *hi.parameters)
            list.add_item().add_paragraph().append_text(format_param(param),
                                                        Paragraph::Kind::InlineCode);
    }

    if(hi.type && !hi.return_type && !hi.parameters) {
        std::string type_str = hi.type->type;
        if(hi.type->aka)
            type_str += " (aka " + *hi.type->aka + ")";
        output.add_paragraph().append_text("Type:").append_text(type_str,
                                                                Paragraph::Kind::InlineCode);
    }

    if(hi.value)
        output.add_paragraph().append_text("Value =").append_text(*hi.value,
                                                                  Paragraph::Kind::InlineCode);

    if(hi.offset)
        output.add_paragraph().append_text("Offset: " + format_offset(*hi.offset));

    if(hi.size) {
        std::string size_text = "Size: " + format_size(*hi.size);
        if(hi.padding && *hi.padding != 0)
            size_text += std::format(" (+{} padding)", format_size(*hi.padding));
        if(hi.align)
            size_text += ", alignment " + format_size(*hi.align);
        output.add_paragraph().append_text(size_text);
    }

    if(!hi.documentation.empty()) {
        output.add_ruler();
        auto [doxygen, rest] = strip_doxygen_info(hi.documentation);

        std::string doc;

        for(auto& [tag, contents]: doxygen.get_block_command_comments()) {
            if(tag == "brief") {
                for(auto& c: contents) {
                    auto text = llvm::StringRef(c.content).trim();
                    if(!text.empty()) {
                        doc += text;
                        doc += "\n\n";
                    }
                }
            }
        }

        auto trimmed = llvm::StringRef(rest).trim();
        if(!trimmed.empty()) {
            doc += trimmed;
            doc += "\n\n";
        }

        auto param_docs = doxygen.get_param_command_comments();
        if(!param_docs.empty()) {
            bool rendered = false;
            if(hi.parameters && !hi.parameters->empty()) {
                for(auto& param: *hi.parameters) {
                    if(!param.name)
                        continue;
                    auto info = doxygen.find_param_info(*param.name);
                    if(!info)
                        continue;
                    auto content = llvm::StringRef((*info)->content).trim();
                    doc += "- `";
                    doc += *param.name;
                    doc += "`";
                    if(!content.empty()) {
                        doc += " — ";
                        doc += content;
                    }
                    doc += "\n";
                    rendered = true;
                }
            }
            if(!rendered) {
                for(auto& [name, info]: param_docs) {
                    auto content = llvm::StringRef(info->content).trim();
                    doc += "- `";
                    doc += name;
                    doc += "`";
                    if(!content.empty()) {
                        doc += " — ";
                        doc += content;
                    }
                    doc += "\n";
                }
            }
            doc += "\n";
        }

        if(auto ret = doxygen.get_return_info()) {
            auto text = ret->trim();
            if(!text.empty()) {
                doc += "**Returns:** ";
                doc += text;
                doc += "\n\n";
            }
        }

        for(auto& [tag, contents]: doxygen.get_block_command_comments()) {
            if(tag == "brief")
                continue;
            std::string label = tag.str();
            if(!label.empty())
                label[0] = std::toupper(label[0]);
            for(auto& c: contents) {
                auto text = llvm::StringRef(c.content).trim();
                if(!text.empty()) {
                    doc += "**";
                    doc += label;
                    doc += ":** ";
                    doc += text;
                    doc += "\n\n";
                }
            }
        }

        auto final_doc = llvm::StringRef(doc).trim();
        if(!final_doc.empty())
            output.add_paragraph().append_text(final_doc.str());
    }

    if(!hi.definition.empty()) {
        output.add_ruler();

        std::string code;
        if(!hi.local_scope.empty()) {
            code += "// In " + llvm::StringRef(hi.local_scope).rtrim(':').str() + '\n';
        } else if(hi.namespace_scope && !hi.namespace_scope->empty()) {
            code +=
                "// In namespace " + llvm::StringRef(*hi.namespace_scope).rtrim(':').str() + '\n';
        }

        if(!hi.access_specifier.empty())
            code += hi.access_specifier + ": ";

        code += hi.definition;
        output.add_code_block(std::move(code), "cpp");
    }

    return output.as_markdown();
}

auto hover_range(CompilationUnitRef unit,
                 const clang::NamedDecl& decl,
                 const PositionMapper& converter) -> std::optional<protocol::Range> {
    auto [fid, range] = unit.decompose_expansion_range(decl.getSourceRange());
    if(fid != unit.interested_file() || !range.valid())
        return std::nullopt;

    return protocol::Range{
        .start = *converter.to_position(range.begin),
        .end = *converter.to_position(range.end),
    };
}

auto get_deduced_type(clang::ASTContext& ctx, clang::SourceLocation loc)
    -> std::optional<clang::QualType> {
    if(!loc.isValid())
        return std::nullopt;

    struct DeducedTypeVisitor : clang::RecursiveASTVisitor<DeducedTypeVisitor> {
        clang::SourceLocation searched;
        clang::QualType deduced;

        DeducedTypeVisitor(clang::SourceLocation loc) : searched(loc) {}

        bool VisitVarDecl(clang::VarDecl* D) {
            if(auto* TSI = D->getTypeSourceInfo()) {
                if(auto ATPTL = TSI->getTypeLoc().getAs<clang::AutoTypeLoc>()) {
                    if(ATPTL.getNameLoc() != searched)
                        return true;
                }
            }
            if(auto* AT = D->getType()->getContainedAutoType()) {
                deduced = AT->desugar();
            }
            return true;
        }

        bool VisitFunctionDecl(clang::FunctionDecl* D) {
            if(!D->getTypeSourceInfo())
                return true;
            auto cur = D->getReturnTypeSourceRange().getBegin();
            if(cur.isInvalid() && llvm::isa<clang::CXXConversionDecl>(D))
                cur = D->getTypeSourceInfo()->getTypeLoc().getBeginLoc();
            if(cur.isInvalid())
                cur = D->getSourceRange().getBegin();
            if(cur != searched)
                return true;

            const clang::AutoType* AT = D->getReturnType()->getContainedAutoType();
            if(AT && !AT->getDeducedType().isNull()) {
                deduced = AT->getDeducedType();
            } else if(auto* DT =
                          llvm::dyn_cast<clang::DecltypeType>(D->getReturnType().getTypePtr())) {
                if(!DT->getUnderlyingType().isNull())
                    deduced = DT->getUnderlyingType();
            } else if(!D->getReturnType().isNull()) {
                deduced = D->getReturnType();
            }
            return true;
        }

        bool VisitDecltypeTypeLoc(clang::DecltypeTypeLoc TL) {
            if(TL.getBeginLoc() != searched)
                return true;
            const clang::DecltypeType* DT = llvm::dyn_cast<clang::DecltypeType>(TL.getTypePtr());
            while(DT && !DT->getUnderlyingType().isNull()) {
                deduced = DT->getUnderlyingType();
                DT = llvm::dyn_cast<clang::DecltypeType>(deduced.getTypePtr());
            }
            return true;
        }
    };

    DeducedTypeVisitor V(loc);
    V.TraverseAST(ctx);
    if(V.deduced.isNull())
        return std::nullopt;
    return V.deduced;
}

auto build_hover(const HoverInfo& hi,
                 CompilationUnitRef unit,
                 const clang::NamedDecl* decl,
                 PositionEncoding encoding) -> protocol::Hover {
    PositionMapper converter(unit.interested_content(), encoding);

    protocol::Hover result{
        .contents =
            protocol::MarkupContent{
                                    .kind = protocol::MarkupKind::Markdown,
                                    .value = present(hi),
                                    },
    };

    if(decl) {
        if(auto range = hover_range(unit, *decl, converter))
            result.range = *range;
    }

    return result;
}

}  // namespace

auto hover(CompilationUnitRef unit,
           const clang::NamedDecl* decl,
           const HoverOptions& options,
           PositionEncoding encoding) -> std::optional<protocol::Hover> {
    if(!decl)
        return std::nullopt;

    auto pp = get_printing_policy(unit.context());
    auto hi = hover_decl(decl, unit, pp, options);
    add_layout_info(*decl, hi);

    return build_hover(hi, unit, decl, encoding);
}

auto hover(CompilationUnitRef unit,
           std::uint32_t offset,
           const HoverOptions& options,
           PositionEncoding encoding) -> std::optional<protocol::Hover> {
    auto& ctx = unit.context();
    auto pp = get_printing_policy(ctx);

    auto file_loc = unit.create_location(unit.interested_file(), offset);
    auto tokens = unit.spelled_tokens_touch(file_loc);

    if(tokens.empty())
        return std::nullopt;

    std::optional<HoverInfo> hi;

    for(const auto& tok: tokens) {
        if(tok.kind() == clang::tok::identifier) {
            if(auto macro_hover = hover_macro(unit, tok.location())) {
                hi = std::move(*macro_hover);
                break;
            }
        } else if(tok.kind() == clang::tok::kw_auto || tok.kind() == clang::tok::kw_decltype) {
            if(auto deduced = get_deduced_type(ctx, tok.location())) {
                hi = hover_deduced_type(*deduced, tok, ctx, pp, options.show_aka);
                break;
            }
            return std::nullopt;
        }
    }

    if(!hi) {
        auto tree = SelectionTree::create_right(unit, LocalSourceRange(offset, offset));
        auto* node = tree.common_ancestor();
        if(!node)
            return std::nullopt;

        const clang::NamedDecl* target_decl = nullptr;

        if(const auto* decl = node->get<clang::NamedDecl>()) {
            target_decl = decl;
        } else if(const auto* ref = node->get<clang::DeclRefExpr>()) {
            target_decl = llvm::dyn_cast<clang::NamedDecl>(ref->getDecl());
        } else if(const auto* member = node->get<clang::MemberExpr>()) {
            target_decl = member->getMemberDecl();
        } else if(const auto* ctor = node->get<clang::CXXConstructExpr>()) {
            target_decl = ctor->getConstructor();
        } else if(const auto* TL = node->get<clang::TypeLoc>()) {
            if(auto* tag_type = TL->getType()->getAs<clang::TagType>())
                target_decl = tag_type->getDecl();
            else if(auto* tdn = TL->getType()->getAs<clang::TypedefType>())
                target_decl = tdn->getDecl();
            else if(auto* tspt = TL->getType()->getAs<clang::TemplateSpecializationType>()) {
                if(auto* TD = tspt->getTemplateName().getAsTemplateDecl())
                    target_decl = TD;
            }
        } else if(const auto* overload = node->get<clang::OverloadExpr>()) {
            if(overload->getNumDecls() == 1)
                target_decl = *overload->decls_begin();
        }

        if(const auto* CTE = node->get<clang::CXXThisExpr>()) {
            hi = hover_this_expr(CTE, ctx, pp, options.show_aka);
        } else if(target_decl) {
            auto info = hover_decl(target_decl, unit, pp, options);
            if(target_decl == node->get<clang::Decl>())
                add_layout_info(*target_decl, info);
            if(!info.value) {
                for(auto* N = node; N; N = N->parent) {
                    if(const clang::Expr* E = N->get<clang::Expr>()) {
                        if(!E->getType().isNull() && E->getType()->isVoidType())
                            break;
                        if(auto val = print_expr_value(E, ctx)) {
                            info.value = std::move(val);
                            break;
                        }
                    } else if(N->get<clang::Decl>() || N->get<clang::Stmt>()) {
                        break;
                    }
                }
            }
            hi = std::move(info);
        } else if(const clang::Expr* E = node->get<clang::Expr>()) {
            if(auto val = print_expr_value(E, ctx)) {
                HoverInfo expr_hi;
                expr_hi.type = print_type(E->getType(), ctx, pp, options.show_aka);
                expr_hi.value = std::move(val);
                expr_hi.name = "expression";
                hi = std::move(expr_hi);
            }
        }
    }

    if(!hi)
        return std::nullopt;

    PositionMapper converter(unit.interested_content(), encoding);
    protocol::Hover result{
        .contents =
            protocol::MarkupContent{
                                    .kind = protocol::MarkupKind::Markdown,
                                    .value = present(*hi),
                                    },
    };

    return result;
}

}  // namespace clice::feature
