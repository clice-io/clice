#include "AST/Utility.h"

#include "Support/Format.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/ScopedPrinter.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"

namespace clice::ast {

bool is_inside_main_file(clang::SourceLocation loc, const clang::SourceManager& sm) {
    if(!loc.isValid())
        return false;
    clang::FileID fid = sm.getFileID(sm.getExpansionLoc(loc));
    return fid == sm.getMainFileID() || fid == sm.getPreambleFileID();
};

bool is_definition(const clang::Decl* decl) {
    if(auto VD = llvm::dyn_cast<clang::VarDecl>(decl)) {
        return VD->isThisDeclarationADefinition();
    }

    if(auto FD = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return FD->isThisDeclarationADefinition();
    }

    if(auto TD = llvm::dyn_cast<clang::TagDecl>(decl)) {
        return TD->isThisDeclarationADefinition();
    }

    if(llvm::isa<clang::FieldDecl,
                 clang::EnumConstantDecl,
                 clang::TypedefNameDecl,
                 clang::ConceptDecl>(decl)) {
        return true;
    }

    return false;
}

bool is_templated(const clang::Decl* decl) {
    if(decl->getDescribedTemplate()) {
        return true;
    }

    if(llvm::isa<clang::TemplateDecl,
                 clang::ClassTemplatePartialSpecializationDecl,
                 clang::VarTemplatePartialSpecializationDecl>(decl)) {
        return true;
    }

    return false;
}

bool is_anonymous(const clang::NamedDecl* decl) {
    auto name = decl->getDeclName();
    return name.isIdentifier() && !name.getAsIdentifierInfo();
}

template <class T>
bool is_template_specialization_kind(const clang::NamedDecl* decl,
                                     clang::TemplateSpecializationKind kind) {
    if(const auto* td = dyn_cast<T>(decl))
        return td->getTemplateSpecializationKind() == kind;
    return false;
}

inline bool is_template_specialization_kind(const clang::NamedDecl* decl,
                                            clang::TemplateSpecializationKind kind) {
    return is_template_specialization_kind<clang::FunctionDecl>(decl, kind) ||
           is_template_specialization_kind<clang::CXXRecordDecl>(decl, kind) ||
           is_template_specialization_kind<clang::VarDecl>(decl, kind);
}

bool is_implicit_template_instantiation(const clang::NamedDecl* decl) {
    return is_template_specialization_kind(decl, clang::TSK_ImplicitInstantiation);
}

const static clang::CXXRecordDecl* getDeclContextForTemplateInstationPattern(const clang::Decl* D) {
    if(const auto* CTSD = dyn_cast<clang::ClassTemplateSpecializationDecl>(D->getDeclContext())) {
        return CTSD->getTemplateInstantiationPattern();
    }

    if(const auto* RD = dyn_cast<clang::CXXRecordDecl>(D->getDeclContext())) {
        return RD->getInstantiatedFromMemberClass();
    }

    return nullptr;
}

const clang::NamedDecl* instantiated_from(const clang::NamedDecl* decl) {
    if(auto CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
        auto kind = CTSD->getTemplateSpecializationKind();
        if(kind == clang::TSK_Undeclared) {
            /// The instantiation of template is lazy, in this case, the specialization is
            /// undeclared. Temporarily return primary template of the specialization.
            /// FIXME: Is there a better way to handle such case?
            return CTSD->getSpecializedTemplate()->getTemplatedDecl();
        } else if(kind == clang::TSK_ExplicitSpecialization) {
            /// If the decl is an full specialization, return itself.
            return CTSD;
        }

        return CTSD->getTemplateInstantiationPattern();
    }

    if(auto FD = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        /// If the decl is an full specialization, return itself.
        if(FD->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return FD;
        }

        return FD->getTemplateInstantiationPattern();
    }

    if(auto VD = llvm::dyn_cast<clang::VarDecl>(decl)) {
        /// If the decl is an full specialization, return itself.
        if(VD->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return VD;
        }

        return VD->getTemplateInstantiationPattern();
    }

    if(auto CRD = llvm::dyn_cast<clang::CXXRecordDecl>(decl)) {
        return CRD->getInstantiatedFromMemberClass();
    }

    /// For `FieldDecl` and `TypedefNameDecl`, clang will not store their instantiation information
    /// in the unit. So we need to look up the original decl manually.
    if(llvm::isa<clang::FieldDecl, clang::TypedefNameDecl>(decl)) {
        /// FIXME: figure out the context.
        if(auto context = getDeclContextForTemplateInstationPattern(decl)) {
            for(auto member: context->lookup(decl->getDeclName())) {
                if(member->isImplicit()) {
                    continue;
                }

                if(member->getKind() == decl->getKind()) {
                    return member;
                }
            }
        }
    }

    if(auto ED = llvm::dyn_cast<clang::EnumDecl>(decl)) {
        return ED->getInstantiatedFromMemberEnum();
    }

    if(auto ECD = llvm::dyn_cast<clang::EnumConstantDecl>(decl)) {
        auto ED = llvm::cast<clang::EnumDecl>(ECD->getDeclContext());
        if(auto context = ED->getInstantiatedFromMemberEnum()) {
            for(auto member: context->lookup(ECD->getDeclName())) {
                return member;
            }
        }
    }

    return nullptr;
}

const clang::NamedDecl* normalize(const clang::NamedDecl* decl) {
    if(!decl) {
        std::abort();
    }

    decl = llvm::cast<clang::NamedDecl>(decl->getCanonicalDecl());

    if(auto ND = instantiated_from(llvm::cast<clang::NamedDecl>(decl))) {
        return llvm::cast<clang::NamedDecl>(ND->getCanonicalDecl());
    }

    return decl;
}

llvm::StringRef simple_name(const clang::DeclarationName& name) {
    if(clang::IdentifierInfo* Ident = name.getAsIdentifierInfo()) {
        return Ident->getName();
    }

    return "";
}

llvm::StringRef identifier_of(const clang::NamedDecl& D) {
    if(clang::IdentifierInfo* identifier = D.getIdentifier()) {
        return identifier->getName();
    }

    return "";
}

llvm::StringRef identifier_of(clang::QualType type) {
    if(const auto* ET = llvm::dyn_cast<clang::ElaboratedType>(type)) {
        return identifier_of(ET->getNamedType());
    }

    if(const auto* BT = llvm::dyn_cast<clang::BuiltinType>(type)) {
        clang::PrintingPolicy PP(clang::LangOptions{});
        PP.adjustForCPlusPlus();
        return BT->getName(PP);
    }

    if(const auto* D = decl_of(type)) {
        return identifier_of(*D);
    }

    return "";
}

std::string name_of(const clang::NamedDecl* decl) {
    llvm::SmallString<128> result;

    auto name = decl->getDeclName();
    switch(name.getNameKind()) {
        case clang::DeclarationName::Identifier: {
            if(auto II = name.getAsIdentifierInfo()) {
                result += name.getAsIdentifierInfo()->getName();
            }
            break;
        }

        case clang::DeclarationName::CXXConstructorName: {
            result += name.getCXXNameType().getAsString();
            break;
        }

        case clang::DeclarationName::CXXDestructorName: {
            result += '~';
            result += name.getCXXNameType().getAsString();
            break;
        }

        case clang::DeclarationName::CXXConversionFunctionName: {
            result += "operator ";
            result += name.getCXXNameType().getAsString();
            break;
        }

        case clang::DeclarationName::CXXOperatorName: {
            result += "operator ";
            result += clang::getOperatorSpelling(name.getCXXOverloadedOperator());
            break;
        }

        case clang::DeclarationName::CXXDeductionGuideName: {
            result += name.getCXXDeductionGuideTemplate()->getNameAsString();
            break;
        }

        case clang::DeclarationName::CXXLiteralOperatorName: {
            result += R"(operator "")";
            result += name.getCXXLiteralIdentifier()->getName();
            break;
        }

        case clang::DeclarationName::CXXUsingDirective: {
            auto UDD = llvm::cast<clang::UsingDirectiveDecl>(decl);
            result += UDD->getNominatedNamespace()->getName();
            break;
        }

        case clang::DeclarationName::ObjCZeroArgSelector:
        case clang::DeclarationName::ObjCOneArgSelector:
        case clang::DeclarationName::ObjCMultiArgSelector: {
            std::unreachable();
        }
    }

    return result.str().str();
}

clang::QualType type_of(const clang::NamedDecl* decl) {
    using namespace clang;

    if(auto value = dyn_cast<ValueDecl>(decl)) {
        if(isa<VarDecl, BindingDecl, FieldDecl, EnumConstantDecl>(value)) {
            return value->getType();
        } else if(auto ctor = dyn_cast<CXXConstructorDecl>(decl)) {
            return ctor->getThisType();
        } else if(auto dtor = dyn_cast<CXXDestructorDecl>(decl)) {
            return dtor->getThisType();
        }
    } else if(auto type = dyn_cast<TypeDecl>(decl)) {
        return QualType(type->getTypeForDecl(), 0);
    }

    return clang::QualType();
}

template <typename Ty>
    requires requires(Ty* T) { T->getDecl(); }
const clang::NamedDecl* decl_of_impl(const Ty* T) {
    return T->getDecl();
}

const clang::NamedDecl* decl_of_impl(const void* T) {
    return nullptr;
}

auto decl_of(clang::QualType type) -> const clang::NamedDecl* {
    if(auto TST = type->getAs<clang::TemplateSpecializationType>()) {
        auto decl = TST->getTemplateName().getAsTemplateDecl();
        if(type->isDependentType()) {
            return decl;
        }

        /// For a template specialization type, the template name is possibly a `ClassTemplateDecl`
        ///  `TypeAliasTemplateDecl` or `TemplateTemplateParmDecl` and `BuiltinTemplateDecl`.
        if(llvm::isa<clang::TypeAliasTemplateDecl>(decl)) {
            return decl->getTemplatedDecl();
        }

        if(llvm::isa<clang::TemplateTemplateParmDecl, clang::BuiltinTemplateDecl>(decl)) {
            return decl;
        }

        return instantiated_from(TST->getAsCXXRecordDecl());
    }

    switch(type->getTypeClass()) {
#define ABSTRACT_TYPE(TY, BASE)
#define TYPE(TY, BASE)                                                                             \
    case clang::Type::TY: return decl_of_impl(llvm::cast<clang::TY##Type>(type));
#include "clang/AST/TypeNodes.inc"
    }

    return nullptr;
}

auto get_qualifier_loc(const clang::NamedDecl* decl) -> clang::NestedNameSpecifierLoc {
    if(auto* V = llvm::dyn_cast<clang::DeclaratorDecl>(decl)) {
        return V->getQualifierLoc();
    }

    if(auto* T = llvm::dyn_cast<clang::TagDecl>(decl)) {
        return T->getQualifierLoc();
    }

    return clang::NestedNameSpecifierLoc();
}

auto get_template_specialization_args(const clang::NamedDecl* decl)
    -> std::optional<llvm::ArrayRef<clang::TemplateArgumentLoc>> {
    if(auto* FD = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        if(const clang::ASTTemplateArgumentListInfo* Args =
               FD->getTemplateSpecializationArgsAsWritten()) {
            return Args->arguments();
        }
    } else if(auto* Cls = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
        if(auto* Args = Cls->getTemplateArgsAsWritten()) {
            return Args->arguments();
        }
    } else if(auto* Var = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(decl)) {
        if(auto* Args = Var->getTemplateArgsAsWritten()) {
            return Args->arguments();
        }
    }

    // We return std::nullopt for ClassTemplateSpecializationDecls because it does
    // not contain TemplateArgumentLoc information.
    return std::nullopt;
}

std::string print_template_specialization_args(const clang::NamedDecl* decl) {
    std::string template_args;
    llvm::raw_string_ostream os(template_args);
    clang::PrintingPolicy policy(decl->getASTContext().getLangOpts());

    if(auto args = get_template_specialization_args(decl)) {
        printTemplateArgumentList(os, *args, policy);
    } else if(auto* cls = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
        // FIXME: Fix cases when getTypeAsWritten returns null inside clang AST,
        // e.g. friend decls. Currently we fallback to Template Arguments without
        // location information.
        printTemplateArgumentList(os, cls->getTemplateArgs().asArray(), policy);
    }

    return template_args;
}

std::string display_name_of(const clang::NamedDecl* decl) {
    std::string name;
    llvm::raw_string_ostream out(name);
    clang::PrintingPolicy policy(decl->getASTContext().getLangOpts());

    // We don't consider a class template's args part of the constructor name.
    policy.SuppressTemplateArgsInCXXConstructors = true;

    // Handle 'using namespace'. They all have the same name - <using-directive>.
    if(auto* UD = llvm::dyn_cast<clang::UsingDirectiveDecl>(decl)) {
        out << "using namespace ";
        if(auto* Qual = UD->getQualifier())
            Qual->print(out, policy);
        UD->getNominatedNamespaceAsWritten()->printName(out);
        return out.str();
    }

    if(is_anonymous(decl)) {
        // Come up with a presentation for an anonymous entity.
        if(llvm::isa<clang::NamespaceDecl>(decl)) {
            return "(anonymous namespace)";
        }

        if(auto* cls = llvm::dyn_cast<clang::RecordDecl>(decl)) {
            if(cls->isLambda()) {
                return "(lambda)";
            }

            return std::format("(anonymous {})", cls->getKindName());
        }

        if(llvm::isa<clang::EnumDecl>(decl)) {
            return "(anonymous enum)";
        }

        return "(anonymous)";
    }

    // Print nested name qualifier if it was written in the source code.
    if(auto* qualifier = get_qualifier_loc(decl).getNestedNameSpecifier()) {
        qualifier->print(out, policy);
    }

    // Print the name itself.
    decl->getDeclName().print(out, policy);
    // Print template arguments.
    out << print_template_specialization_args(decl);

    return out.str();
}

auto unwrap_type(clang::TypeLoc type, bool unwrap_function_type) -> clang::TypeLoc {
    while(true) {
        if(auto qualified = type.getAs<clang::QualifiedTypeLoc>()) {
            type = qualified.getUnqualifiedLoc();
        } else if(auto reference = type.getAs<clang::ReferenceTypeLoc>()) {
            type = reference.getPointeeLoc();
        } else if(auto pointer = type.getAs<clang::PointerTypeLoc>()) {
            type = pointer.getPointeeLoc();
        } else if(auto paren = type.getAs<clang::ParenTypeLoc>()) {
            type = paren.getInnerLoc();
        } else if(auto array = type.getAs<clang::ConstantArrayTypeLoc>()) {
            type = array.getElementLoc();
        } else if(auto proto = type.getAs<clang::FunctionProtoTypeLoc>();
                  proto && unwrap_function_type) {
            type = proto.getReturnLoc();
        } else {
            break;
        }
    }
    return type;
}

template <typename TemplateDeclTy>
static clang::NamedDecl* get_only_instantiation_impl(TemplateDeclTy* TD) {
    clang::NamedDecl* Only = nullptr;
    for(auto* Spec: TD->specializations()) {
        if(Spec->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization)
            continue;
        if(Only != nullptr)
            return nullptr;
        Only = Spec;
    }
    return Only;
}

clang::NamedDecl* get_only_instantiation(clang::NamedDecl* TemplatedDecl) {
    if(auto* TD = TemplatedDecl->getDescribedTemplate()) {
        if(auto* CTD = llvm::dyn_cast<clang::ClassTemplateDecl>(TD))
            return get_only_instantiation_impl(CTD);
        if(auto* FTD = llvm::dyn_cast<clang::FunctionTemplateDecl>(TD))
            return get_only_instantiation_impl(FTD);
        if(auto* VTD = llvm::dyn_cast<clang::VarTemplateDecl>(TD))
            return get_only_instantiation_impl(VTD);
    }
    return nullptr;
}

auto get_only_instantiation(clang::ParmVarDecl* decl) -> clang::ParmVarDecl* {
    auto* TemplateFunction = llvm::dyn_cast<clang::FunctionDecl>(decl->getDeclContext());
    if(!TemplateFunction)
        return nullptr;
    auto* InstantiatedFunction =
        llvm::dyn_cast_or_null<clang::FunctionDecl>(get_only_instantiation(TemplateFunction));
    if(!InstantiatedFunction)
        return nullptr;

    unsigned ParamIdx = 0;
    for(auto* Param: TemplateFunction->parameters()) {
        // Can't reason about param indexes in the presence of preceding packs.
        // And if this param is a pack, it may expand to multiple params.
        if(Param->isParameterPack())
            return nullptr;
        if(Param == decl)
            break;
        ++ParamIdx;
    }
    assert(ParamIdx < TemplateFunction->getNumParams() && "Couldn't find param in list?");
    assert(ParamIdx < InstantiatedFunction->getNumParams() &&
           "Instantiated function has fewer (non-pack) parameters?");
    return InstantiatedFunction->getParamDecl(ParamIdx);
}

std::string summarize_expr(const clang::Expr* E) {
    using namespace clang;

    struct Namer : ConstStmtVisitor<Namer, std::string> {
        std::string Visit(const Expr* E) {
            if(E == nullptr)
                return "";
            return ConstStmtVisitor::Visit(E->IgnoreImplicit());
        }

        // Any sort of decl reference, we just use the unqualified name.
        std::string VisitMemberExpr(const MemberExpr* E) {
            return identifier_of(*E->getMemberDecl()).str();
        }

        std::string VisitDeclRefExpr(const DeclRefExpr* E) {
            return identifier_of(*E->getFoundDecl()).str();
        }

        std::string VisitCallExpr(const CallExpr* E) {
            return Visit(E->getCallee());
        }

        std::string VisitCXXDependentScopeMemberExpr(const CXXDependentScopeMemberExpr* E) {
            return simple_name(E->getMember()).str();
        }

        std::string VisitDependentScopeDeclRefExpr(const DependentScopeDeclRefExpr* E) {
            return simple_name(E->getDeclName()).str();
        }

        std::string VisitCXXFunctionalCastExpr(const CXXFunctionalCastExpr* E) {
            return identifier_of(E->getType()).str();
        }

        std::string VisitCXXTemporaryObjectExpr(const CXXTemporaryObjectExpr* E) {
            return identifier_of(E->getType()).str();
        }

        // Step through implicit nodes that clang doesn't classify as such.
        std::string VisitCXXMemberCallExpr(const CXXMemberCallExpr* E) {
            // Call to operator bool() inside if (X): dispatch to X.
            if(E->getNumArgs() == 0 && E->getMethodDecl() &&
               E->getMethodDecl()->getDeclName().getNameKind() ==
                   DeclarationName::CXXConversionFunctionName &&
               E->getSourceRange() == E->getImplicitObjectArgument()->getSourceRange())
                return Visit(E->getImplicitObjectArgument());
            return ConstStmtVisitor::VisitCXXMemberCallExpr(E);
        }

        std::string VisitCXXConstructExpr(const CXXConstructExpr* E) {
            if(E->getNumArgs() == 1)
                return Visit(E->getArg(0));
            return "";
        }

        // Literals are just printed
        std::string VisitCXXBoolLiteralExpr(const CXXBoolLiteralExpr* E) {
            return E->getValue() ? "true" : "false";
        }

        std::string VisitIntegerLiteral(const IntegerLiteral* E) {
            return llvm::to_string(E->getValue());
        }

        std::string VisitFloatingLiteral(const FloatingLiteral* E) {
            std::string Result;
            llvm::raw_string_ostream OS(Result);
            E->getValue().print(OS);
            // Printer adds newlines?!
            Result.resize(llvm::StringRef(Result).rtrim().size());
            return Result;
        }

        std::string VisitStringLiteral(const StringLiteral* E) {
            std::string Result = "\"";
            if(E->containsNonAscii()) {
                Result += "...";
            } else if(E->getLength() > 10) {
                Result += E->getString().take_front(7);
                Result += "...";
            } else {
                llvm::raw_string_ostream OS(Result);
                llvm::printEscapedString(E->getString(), OS);
            }
            Result.push_back('"');
            return Result;
        }

        // Simple operators. Motivating cases are `!x` and `I < Length`.
        std::string printUnary(llvm::StringRef Spelling, const Expr* Operand, bool Prefix) {
            std::string Sub = Visit(Operand);
            if(Sub.empty())
                return "";
            if(Prefix)
                return (Spelling + Sub).str();
            Sub += Spelling;
            return Sub;
        }

        bool InsideBinary = false;  // No recursing into binary expressions.

        std::string printBinary(llvm::StringRef Spelling, const Expr* LHSOp, const Expr* RHSOp) {
            if(InsideBinary)
                return "";
            llvm::SaveAndRestore InBinary(InsideBinary, true);

            std::string LHS = Visit(LHSOp);
            std::string RHS = Visit(RHSOp);
            if(LHS.empty() && RHS.empty())
                return "";

            if(LHS.empty())
                LHS = "...";
            LHS.push_back(' ');
            LHS += Spelling;
            LHS.push_back(' ');
            if(RHS.empty())
                LHS += "...";
            else
                LHS += RHS;
            return LHS;
        }

        std::string VisitUnaryOperator(const UnaryOperator* E) {
            return printUnary(E->getOpcodeStr(E->getOpcode()), E->getSubExpr(), !E->isPostfix());
        }

        std::string VisitBinaryOperator(const BinaryOperator* E) {
            return printBinary(E->getOpcodeStr(E->getOpcode()), E->getLHS(), E->getRHS());
        }

        std::string VisitCXXOperatorCallExpr(const CXXOperatorCallExpr* E) {
            const char* Spelling = getOperatorSpelling(E->getOperator());
            // Handle weird unary-that-look-like-binary postfix operators.
            if((E->getOperator() == OO_PlusPlus || E->getOperator() == OO_MinusMinus) &&
               E->getNumArgs() == 2)
                return printUnary(Spelling, E->getArg(0), false);
            if(E->isInfixBinaryOp())
                return printBinary(Spelling, E->getArg(0), E->getArg(1));
            if(E->getNumArgs() == 1) {
                switch(E->getOperator()) {
                    case OO_Plus:
                    case OO_Minus:
                    case OO_Star:
                    case OO_Amp:
                    case OO_Tilde:
                    case OO_Exclaim:
                    case OO_PlusPlus:
                    case OO_MinusMinus: return printUnary(Spelling, E->getArg(0), true);
                    default: break;
                }
            }
            return "";
        }
    };

    return Namer{}.Visit(E);
}

// Returns the template parameter pack type from an instantiated function
// template, if it exists, nullptr otherwise.
const clang::TemplateTypeParmType* function_pack_type(const clang::FunctionDecl* callee) {
    // returns true for `X` in `template <typename... X> void foo()`
    auto is_type_pack = [](clang::NamedDecl* decl) {
        if(const auto* TTPD = llvm::dyn_cast<clang::TemplateTypeParmDecl>(decl)) {
            return TTPD->isParameterPack();
        }
        return false;
    };

    if(const auto* decl = callee->getPrimaryTemplate()) {
        auto template_params = decl->getTemplateParameters()->asArray();
        // find the template parameter pack from the back
        const auto it =
            std::ranges::find_if(template_params.rbegin(), template_params.rend(), is_type_pack);
        if(it != template_params.rend()) {
            const auto* TTPD = llvm::dyn_cast<clang::TemplateTypeParmDecl>(*it);
            return TTPD->getTypeForDecl()->castAs<clang::TemplateTypeParmType>();
        }
    }

    return nullptr;
}

// Returns the template parameter pack type that this parameter was expanded
// from (if in the Args... or Args&... or Args&&... form), if this is the case,
// nullptr otherwise.
const clang::TemplateTypeParmType* underlying_pack_type(const clang::ParmVarDecl* param) {
    const auto* type = param->getType().getTypePtr();
    if(auto* ref_type = llvm::dyn_cast<clang::ReferenceType>(type)) {
        type = ref_type->getPointeeTypeAsWritten().getTypePtr();
    }

    if(const auto* subst_type = llvm::dyn_cast<clang::SubstTemplateTypeParmType>(type)) {
        const auto* decl = subst_type->getReplacedParameter();
        if(decl->isParameterPack()) {
            return decl->getTypeForDecl()->castAs<clang::TemplateTypeParmType>();
        }
    }

    return nullptr;
}

// This visitor walks over the body of an instantiated function template.
// The template accepts a parameter pack and the visitor records whether
// the pack parameters were forwarded to another call. For example, given:
//
// template <typename T, typename... Args>
// auto make_unique(Args... args) {
//   return unique_ptr<T>(new T(args...));
// }
//
// When called as `make_unique<std::string>(2, 'x')` this yields a function
// `make_unique<std::string, int, char>` with two parameters.
// The visitor records that those two parameters are forwarded to the
// `constructor std::string(int, char);`.
//
// This information is recorded in the `ForwardingInfo` split into fully
// resolved parameters (passed as argument to a parameter that is not an
// expanded template type parameter pack) and forwarding parameters (passed to a
// parameter that is an expanded template type parameter pack).
class ForwardingCallVisitor : public clang::RecursiveASTVisitor<ForwardingCallVisitor> {
public:
    ForwardingCallVisitor(llvm::ArrayRef<const clang::ParmVarDecl*> Parameters) :
        Parameters{Parameters}, PackType{underlying_pack_type(Parameters.front())} {}

    bool VisitCallExpr(clang::CallExpr* E) {
        auto* Callee = getCalleeDeclOrUniqueOverload(E);
        if(Callee) {
            handleCall(Callee, E->arguments());
        }
        return !Info.has_value();
    }

    bool VisitCXXConstructExpr(clang::CXXConstructExpr* E) {
        auto* Callee = E->getConstructor();
        if(Callee) {
            handleCall(Callee, E->arguments());
        }
        return !Info.has_value();
    }

    // The expanded parameter pack to be resolved
    llvm::ArrayRef<const clang::ParmVarDecl*> Parameters;
    // The type of the parameter pack
    const clang::TemplateTypeParmType* PackType;

    struct ForwardingInfo {
        // If the parameters were resolved to another FunctionDecl, these are its
        // first non-variadic parameters (i.e. the first entries of the parameter
        // pack that are passed as arguments bound to a non-pack parameter.)
        llvm::ArrayRef<const clang::ParmVarDecl*> Head;
        // If the parameters were resolved to another FunctionDecl, these are its
        // variadic parameters (i.e. the entries of the parameter pack that are
        // passed as arguments bound to a pack parameter.)
        llvm::ArrayRef<const clang::ParmVarDecl*> Pack;
        // If the parameters were resolved to another FunctionDecl, these are its
        // last non-variadic parameters (i.e. the last entries of the parameter pack
        // that are passed as arguments bound to a non-pack parameter.)
        llvm::ArrayRef<const clang::ParmVarDecl*> Tail;
        // If the parameters were resolved to another FunctionDecl, this
        // is it.
        std::optional<clang::FunctionDecl*> PackTarget;
    };

    // The output of this visitor
    std::optional<ForwardingInfo> Info;

private:
    // inspects the given callee with the given args to check whether it
    // contains Parameters, and sets Info accordingly.
    void handleCall(clang::FunctionDecl* Callee, typename clang::CallExpr::arg_range Args) {
        // Skip functions with less parameters, they can't be the target.
        if(Callee->parameters().size() < Parameters.size())
            return;
        if(llvm::any_of(Args,
                        [](const clang::Expr* E) { return isa<clang::PackExpansionExpr>(E); })) {
            return;
        }
        auto PackLocation = findPack(Args);
        if(!PackLocation)
            return;
        llvm::ArrayRef<clang::ParmVarDecl*> MatchingParams =
            Callee->parameters().slice(*PackLocation, Parameters.size());
        // Check whether the function has a parameter pack as the last template
        // parameter
        if(const auto* TTPT = function_pack_type(Callee)) {
            // In this case: Separate the parameters into head, pack and tail
            auto IsExpandedPack = [&](const clang::ParmVarDecl* P) {
                return underlying_pack_type(P) == TTPT;
            };
            ForwardingInfo FI;
            FI.Head = MatchingParams.take_until(IsExpandedPack);
            FI.Pack = MatchingParams.drop_front(FI.Head.size()).take_while(IsExpandedPack);
            FI.Tail = MatchingParams.drop_front(FI.Head.size() + FI.Pack.size());
            FI.PackTarget = Callee;
            Info = FI;
            return;
        }
        // Default case: assume all parameters were fully resolved
        ForwardingInfo FI;
        FI.Head = MatchingParams;
        Info = FI;
    }

    // Returns the beginning of the expanded pack represented by Parameters
    // in the given arguments, if it is there.
    std::optional<size_t> findPack(typename clang::CallExpr::arg_range Args) {
        // find the argument directly referring to the first parameter
        assert(Parameters.size() <= static_cast<size_t>(llvm::size(Args)));
        for(auto Begin = Args.begin(), End = Args.end() - Parameters.size() + 1; Begin != End;
            ++Begin) {
            if(const auto* RefArg = unwrapForward(*Begin)) {
                if(Parameters.front() != RefArg->getDecl())
                    continue;
                // Check that this expands all the way until the last parameter.
                // It's enough to look at the last parameter, because it isn't possible
                // to expand without expanding all of them.
                auto ParamEnd = Begin + Parameters.size() - 1;
                RefArg = unwrapForward(*ParamEnd);
                if(!RefArg || Parameters.back() != RefArg->getDecl())
                    continue;
                return std::distance(Args.begin(), Begin);
            }
        }
        return std::nullopt;
    }

    static clang::FunctionDecl* getCalleeDeclOrUniqueOverload(clang::CallExpr* E) {
        clang::Decl* CalleeDecl = E->getCalleeDecl();
        auto* Callee = llvm::dyn_cast_or_null<clang::FunctionDecl>(CalleeDecl);
        if(!Callee) {
            if(auto* Lookup = dyn_cast<clang::UnresolvedLookupExpr>(E->getCallee())) {
                Callee = resolveOverload(Lookup, E);
            }
        }
        // Ignore the callee if the number of arguments is wrong (deal with va_args)
        if(Callee && Callee->getNumParams() == E->getNumArgs())
            return Callee;
        return nullptr;
    }

    static clang::FunctionDecl* resolveOverload(clang::UnresolvedLookupExpr* Lookup,
                                                clang::CallExpr* E) {
        clang::FunctionDecl* MatchingDecl = nullptr;
        if(!Lookup->requiresADL()) {
            // Check whether there is a single overload with this number of
            // parameters
            for(auto* Candidate: Lookup->decls()) {
                if(auto* FuncCandidate = llvm::dyn_cast_or_null<clang::FunctionDecl>(Candidate)) {
                    if(FuncCandidate->getNumParams() == E->getNumArgs()) {
                        if(MatchingDecl) {
                            // there are multiple candidates - abort
                            return nullptr;
                        }
                        MatchingDecl = FuncCandidate;
                    }
                }
            }
        }
        return MatchingDecl;
    }

    // Tries to get to the underlying argument by unwrapping implicit nodes and
    // std::forward.
    const static clang::DeclRefExpr* unwrapForward(const clang::Expr* E) {
        E = E->IgnoreImplicitAsWritten();
        // There might be an implicit copy/move constructor call on top of the
        // forwarded arg.
        // FIXME: Maybe mark implicit calls in the AST to properly filter here.
        if(const auto* Const = llvm::dyn_cast<clang::CXXConstructExpr>(E))
            if(Const->getConstructor()->isCopyOrMoveConstructor())
                E = Const->getArg(0)->IgnoreImplicitAsWritten();
        if(const auto* Call = llvm::dyn_cast<clang::CallExpr>(E)) {
            const auto Callee = Call->getBuiltinCallee();
            if(Callee == clang::Builtin::BIforward) {
                return llvm::dyn_cast<clang::DeclRefExpr>(
                    Call->getArg(0)->IgnoreImplicitAsWritten());
            }
        }
        return llvm::dyn_cast<clang::DeclRefExpr>(E);
    }
};

auto resolve_forwarding_params(const clang::FunctionDecl* D, unsigned MaxDepth)
    -> llvm::SmallVector<const clang::ParmVarDecl*> {
    auto params = D->parameters();

    // If the function has a template parameter pack
    if(const auto* TTPT = function_pack_type(D)) {
        // Split the parameters into head, pack and tail
        auto IsExpandedPack = [TTPT](const clang::ParmVarDecl* P) {
            return underlying_pack_type(P) == TTPT;
        };
        llvm::ArrayRef<const clang::ParmVarDecl*> Head = params.take_until(IsExpandedPack);
        llvm::ArrayRef<const clang::ParmVarDecl*> Pack =
            params.drop_front(Head.size()).take_while(IsExpandedPack);
        llvm::ArrayRef<const clang::ParmVarDecl*> Tail =
            params.drop_front(Head.size() + Pack.size());
        llvm::SmallVector<const clang::ParmVarDecl*> Result(params.size());
        // Fill in non-pack parameters
        auto* HeadIt = std::copy(Head.begin(), Head.end(), Result.begin());
        auto TailIt = std::copy(Tail.rbegin(), Tail.rend(), Result.rbegin());
        // Recurse on pack parameters

        size_t Depth = 0;

        const clang::FunctionDecl* CurrentFunction = D;
        llvm::SmallSet<const clang::FunctionTemplateDecl*, 4> SeenTemplates;
        if(const auto* Template = D->getPrimaryTemplate()) {
            SeenTemplates.insert(Template);
        }

        while(!Pack.empty() && CurrentFunction && Depth < MaxDepth) {
            // Find call expressions involving the pack
            ForwardingCallVisitor V{Pack};
            V.TraverseStmt(CurrentFunction->getBody());
            if(!V.Info) {
                break;
            }
            // If we found something: Fill in non-pack parameters
            auto Info = *V.Info;
            HeadIt = std::copy(Info.Head.begin(), Info.Head.end(), HeadIt);
            TailIt = std::copy(Info.Tail.rbegin(), Info.Tail.rend(), TailIt);
            // Prepare next recursion level
            Pack = Info.Pack;
            CurrentFunction = Info.PackTarget.value_or(nullptr);
            Depth++;
            // If we are recursing into a previously encountered function: Abort
            if(CurrentFunction) {
                if(const auto* Template = CurrentFunction->getPrimaryTemplate()) {
                    bool NewFunction = SeenTemplates.insert(Template).second;
                    if(!NewFunction) {
                        return {params.begin(), params.end()};
                    }
                }
            }
        }

        // Fill in the remaining unresolved pack parameters
        HeadIt = std::copy(Pack.begin(), Pack.end(), HeadIt);
        assert(TailIt.base() == HeadIt);
        return Result;
    }
    return {params.begin(), params.end()};
}

// Determines if any intermediate type in desugaring QualType QT is of
// substituted template parameter type. Ignore pointer or reference wrappers.
static bool isSugaredTemplateParameter(clang::QualType type) {
    static auto peel_wrapper = [](clang::QualType type) {
        // Neither `PointerType` nor `ReferenceType` is considered as sugared
        // type. Peel it.
        clang::QualType peeled = type->getPointeeType();
        return peeled.isNull() ? type : peeled;
    };

    // This is a bit tricky: we traverse the type structure and find whether or
    // not a type in the desugaring process is of SubstTemplateTypeParmType.
    // During the process, we may encounter pointer or reference types that are
    // not marked as sugared; therefore, the desugar function won't apply. To
    // move forward the traversal, we retrieve the pointees using
    // QualType::getPointeeType().
    //
    // However, getPointeeType could leap over our interests: The QT::getAs<T>()
    // invoked would implicitly desugar the type. Consequently, if the
    // SubstTemplateTypeParmType is encompassed within a TypedefType, we may lose
    // the chance to visit it.
    // For example, given a QT that represents `std::vector<int *>::value_type`:
    //  `-ElaboratedType 'value_type' sugar
    //    `-TypedefType 'vector<int *>::value_type' sugar
    //      |-Typedef 'value_type'
    //      `-SubstTemplateTypeParmType 'int *' sugar class depth 0 index 0 T
    //        |-ClassTemplateSpecialization 'vector'
    //        `-PointerType 'int *'
    //          `-BuiltinType 'int'
    // Applying `getPointeeType` to QT results in 'int', a child of our target
    // node SubstTemplateTypeParmType.
    //
    // As such, we always prefer the desugared over the pointee for next type
    // in the iteration. It could avoid the getPointeeType's implicit desugaring.
    while(true) {
        if(type->getAs<clang::SubstTemplateTypeParmType>()) {
            return true;
        }

        clang::QualType desugared = type->getLocallyUnqualifiedSingleStepDesugaredType();
        if(desugared != type) {
            type = desugared;
        } else if(auto peeled = peel_wrapper(desugared); peeled != type) {
            type = peeled;
        } else {
            break;
        }
    }

    return false;
}

std::optional<clang::QualType> desugar(clang::ASTContext& context, clang::QualType type) {
    bool ShouldAKA = false;
    auto Desugared = clang::desugarForDiagnostic(context, type, ShouldAKA);
    if(!ShouldAKA) {
        return std::nullopt;
    }

    return Desugared;
}

clang::QualType maybe_desugar(clang::ASTContext& context, clang::QualType type) {
    // Prefer desugared type for name that aliases the template parameters.
    // This can prevent things like printing opaque `: type` when accessing std
    // containers.
    if(isSugaredTemplateParameter(type)) {
        return desugar(context, type).value_or(type);
    }

    // Prefer desugared type for `decltype(expr)` specifiers.
    if(type->isDecltypeType()) {
        return type.getCanonicalType();
    }

    if(const auto* AT = type->getContainedAutoType()) {
        if(!AT->getDeducedType().isNull() && AT->getDeducedType()->isDecltypeType()) {
            return type.getCanonicalType();
        }
    }

    return type;
}

clang::FunctionProtoTypeLoc proto_type_loc(clang::Expr* expr) {
    clang::TypeLoc target;
    clang::Expr* naked_fn = expr->IgnoreParenCasts();

    if(const auto* T = naked_fn->getType().getTypePtr()->getAs<clang::TypedefType>()) {
        target = T->getDecl()->getTypeSourceInfo()->getTypeLoc();
    } else if(const auto* DR = llvm::dyn_cast<clang::DeclRefExpr>(naked_fn)) {
        const auto* D = DR->getDecl();
        if(const auto* const VD = llvm::dyn_cast<clang::VarDecl>(D)) {
            target = VD->getTypeSourceInfo()->getTypeLoc();
        }
    }

    if(!target) {
        return {};
    }

    // Unwrap types that may be wrapping the function type
    while(true) {
        if(auto p = target.getAs<clang::PointerTypeLoc>()) {
            target = p.getPointeeLoc();
            continue;
        }

        if(auto a = target.getAs<clang::AttributedTypeLoc>()) {
            target = a.getModifiedLoc();
            continue;
        }

        if(auto p = target.getAs<clang::ParenTypeLoc>()) {
            target = p.getInnerLoc();
            continue;
        }

        break;
    }

    if(auto f = target.getAs<clang::FunctionProtoTypeLoc>()) {
        return f;
    }

    return {};
}

}  // namespace clice::ast
