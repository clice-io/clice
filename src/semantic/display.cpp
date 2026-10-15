module;

#include "modules/prelude.h"

/// Parts of this file are ported from clangd's AST.cpp, Hover.cpp,
/// InlayHints.cpp and CodeCompletionStrings.cpp (llvmorg-21.1.8), part of
/// the LLVM project, licensed under Apache License v2.0 with LLVM
/// Exceptions. See https://llvm.org/LICENSE.txt for license information.

module clice;

import :semantic.decls;
import :semantic.display;
import :semantic.types;
import :support.format;

namespace clice::display {

namespace {

llvm::StringRef simple_name(const clang::DeclarationName& name) {
    if(clang::IdentifierInfo* identifier = name.getAsIdentifierInfo()) {
        return identifier->getName();
    }

    return "";
}

/// The identifier text of the decl (or builtin) behind a type, empty
/// otherwise.
llvm::StringRef type_identifier(clang::QualType type) {
    if(const auto* builtin = llvm::dyn_cast<clang::BuiltinType>(type)) {
        clang::PrintingPolicy pp(clang::LangOptions{});
        pp.adjustForCPlusPlus();
        return builtin->getName(pp);
    }

    if(const auto* decl = types::decl_of(type)) {
        return simple_name(decl->getDeclName());
    }

    return "";
}

/// Names a class as the code of a scope names it: without the namespaces
/// and classes the scope is in too.
struct EnclosingScopes : clang::PrintingCallbacks {
    explicit EnclosingScopes(const clang::DeclContext* scope) : scope(scope) {}

    bool isScopeVisible(const clang::DeclContext* outer) const override {
        return outer->Encloses(scope);
    }

    const clang::DeclContext* scope;
};

bool is_anonymous_name(const clang::DeclarationName& name) {
    return name.isIdentifier() && !name.getAsIdentifierInfo();
}

auto qualifier_loc(const clang::NamedDecl& decl) -> clang::NestedNameSpecifierLoc {
    if(auto* declarator = llvm::dyn_cast<clang::DeclaratorDecl>(&decl)) {
        return declarator->getQualifierLoc();
    }

    if(auto* tag = llvm::dyn_cast<clang::TagDecl>(&decl)) {
        return tag->getQualifierLoc();
    }

    return clang::NestedNameSpecifierLoc();
}

auto template_specialization_arg_locs(const clang::NamedDecl& decl)
    -> std::optional<llvm::ArrayRef<clang::TemplateArgumentLoc>> {
    if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(&decl)) {
        if(const auto* args = function->getTemplateSpecializationArgsAsWritten()) {
            return args->arguments();
        }
    } else if(auto* record = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&decl)) {
        if(const auto* args = record->getTemplateArgsAsWritten()) {
            return args->arguments();
        }
    } else if(auto* var = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(&decl)) {
        if(const auto* args = var->getTemplateArgsAsWritten()) {
            return args->arguments();
        }
    }

    /// We return std::nullopt for implicit specializations because they do
    /// not contain TemplateArgumentLoc information.
    return std::nullopt;
}

}  // namespace

std::string template_args(const clang::NamedDecl& decl) {
    std::string args;
    llvm::raw_string_ostream os(args);
    clang::PrintingPolicy policy(decl.getASTContext().getLangOpts());
    if(auto arg_locs = template_specialization_arg_locs(decl)) {
        clang::printTemplateArgumentList(os, *arg_locs, policy);
    } else if(auto* function = llvm::dyn_cast<clang::FunctionDecl>(&decl)) {
        /// An explicit specialization's written arguments are not always
        /// recorded; the converted ones spell the same specialization. An
        /// explicitly specialized member of a class template reports the
        /// same kind but has no argument list of its own.
        if(const auto* args = function->getTemplateSpecializationArgs();
           args && function->getTemplateSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            clang::printTemplateArgumentList(os, args->asArray(), policy);
        }
    } else if(auto* record = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&decl)) {
        /// FIXME: Fix cases when getTypeAsWritten returns null inside clang
        /// AST, e.g. friend decls. Currently we fallback to template
        /// arguments without location information.
        clang::printTemplateArgumentList(os, record->getTemplateArgs().asArray(), policy);
    } else if(auto* var = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(&decl)) {
        /// Implicit variable template specializations carry no written
        /// arguments; fall back to the converted ones.
        clang::printTemplateArgumentList(os, var->getTemplateArgs().asArray(), policy);
    }
    return args;
}

auto specialization_args(const clang::NamedDecl& decl, const Options& options) -> std::string {
    if(llvm::isa<clang::ClassTemplatePartialSpecializationDecl,
                 clang::VarTemplatePartialSpecializationDecl>(decl)) {
        return template_args(decl);
    }

    auto& context = decl.getASTContext();
    llvm::ArrayRef<clang::TemplateArgument> args;
    const clang::TemplateParameterList* params = nullptr;
    if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&decl)) {
        args = spec->getTemplateArgs().asArray();
        params = spec->getSpecializedTemplate()->getTemplateParameters();
        // The readable type keeps its arguments in a template-id; one the
        // standard has an alias for keeps them behind it.
        if(options.readable) {
            auto readable = options.readable->readable(context.getCanonicalTagType(spec));
            if(const auto* id = readable->getAs<clang::TemplateSpecializationType>()) {
                args = id->template_arguments();
            }
        }
    } else if(const auto* var = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(&decl)) {
        args = var->getTemplateArgs().asArray();
        params = var->getSpecializedTemplate()->getTemplateParameters();
    } else {
        return {};
    }

    EnclosingScopes scopes(decl.getDeclContext());
    clang::PrintingPolicy policy(context.getLangOpts());
    policy.Callbacks = &scopes;
    std::string text;
    llvm::raw_string_ostream os(text);
    clang::printTemplateArgumentList(os, args, policy, params);
    return text;
}

namespace {

// Determines if any intermediate type in desugaring QualType QT is of
// substituted template parameter type. Ignore pointer or reference wrappers.
bool is_sugared_template_parameter(clang::QualType type) {
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
    //  `-TypedefType 'vector<int *>::value_type' sugar
    //    |-Typedef 'value_type'
    //    `-SubstTemplateTypeParmType 'int *' sugar class depth 0 index 0 T
    //      |-ClassTemplateSpecialization 'vector'
    //      `-PointerType 'int *'
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

/// A simple wrapper for `clang::desugarForDiagnostic` that provides optional
/// semantic.
std::optional<clang::QualType> desugar(clang::ASTContext& context, clang::QualType type) {
    bool should_aka = false;
    auto desugared = clang::desugarForDiagnostic(context, type, should_aka);
    if(!should_aka) {
        return std::nullopt;
    }

    return desugared;
}

}  // namespace

auto identifier_of(const clang::NamedDecl* decl) -> llvm::StringRef {
    assert(decl);
    if(auto* identifier = decl->getIdentifier()) {
        return identifier->getName();
    }
    return "";
}

auto identifier_of(const clang::Expr* expr) -> llvm::StringRef {
    assert(expr);
    expr = expr->IgnoreUnlessSpelledInSource();

    if(auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
        if(!ref->getQualifier()) {
            return identifier_of(ref->getDecl());
        }
    }

    if(auto* member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
        if(!member->getQualifier() && member->isImplicitAccess()) {
            return identifier_of(member->getMemberDecl());
        }
    }

    return "";
}

namespace {

/// The bare declaration name, printed with the hand-rolled switch the bare
/// form historically used. It diverges from DeclarationName::print in a few
/// corners (deduction guides print the plain template name, literal
/// operators keep a space after `operator ""`); unifying the two printers
/// is a deliberate, snapshot-reviewed change.
std::string bare_name(const clang::NamedDecl* decl, const Options& options) {
    llvm::SmallString<128> result;

    /// Use the language options of the declaration's context so that C++ types
    /// print without their tag keyword, e.g. "~Account" instead of "~struct Account".
    clang::PrintingPolicy policy(decl->getASTContext().getLangOpts());

    auto name = decl->getDeclName();
    switch(name.getNameKind()) {
        case clang::DeclarationName::Identifier: {
            if(auto* identifier = name.getAsIdentifierInfo()) {
                result += identifier->getName();
            }
            break;
        }

        case clang::DeclarationName::CXXConstructorName:
        case clang::DeclarationName::CXXDestructorName: {
            if(name.getNameKind() == clang::DeclarationName::CXXDestructorName) {
                result += '~';
            }
            auto type = name.getCXXNameType();
            /// The type prints with its scope and, for a class template's
            /// own members, with the template's parameters; the option asks
            /// for the class's name alone.
            auto* record = options.suppress_ctor_template_args ? type->getAsRecordDecl() : nullptr;
            if(record) {
                result += record->getName();
            } else {
                result += type.getAsString(policy);
            }
            break;
        }

        case clang::DeclarationName::CXXConversionFunctionName: {
            result += "operator ";
            /// The name holds the canonical type (`operator int *` for
            /// `operator Handle`); the declaration keeps the type as written.
            auto type = name.getCXXNameType();
            if(auto* conversion = llvm::dyn_cast<clang::CXXConversionDecl>(decl)) {
                type = conversion->getConversionType();
            }
            result += type.getAsString(policy);
            break;
        }

        case clang::DeclarationName::CXXOperatorName: {
            llvm::StringRef spelling = clang::getOperatorSpelling(name.getCXXOverloadedOperator());
            result += "operator";
            /// Mirror DeclarationName::print: separate with a space only for
            /// word-like operators such as "new", "delete" and "co_await".
            if(clang::isLowercase(spelling.front())) {
                result += ' ';
            }
            result += spelling;
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

}  // namespace

auto name_of(const clang::NamedDecl* decl, const Options& options) -> std::string {
    assert(decl);

    if(!options.qualified) {
        return bare_name(decl, options);
    }

    std::string name;
    llvm::raw_string_ostream os(name);
    clang::PrintingPolicy policy(decl->getASTContext().getLangOpts());
    /// We don't consider a class template's args part of the constructor name.
    policy.SuppressTemplateArgsInCXXConstructors = true;

    /// Handle 'using namespace'. They all have the same name - <using-directive>.
    if(auto* directive = llvm::dyn_cast<clang::UsingDirectiveDecl>(decl)) {
        os << "using namespace ";
        if(auto qualifier = directive->getQualifier()) {
            qualifier.print(os, policy);
        }
        directive->getNominatedNamespaceAsWritten()->printName(os);
        return name;
    }

    /// Come up with a presentation for an anonymous entity.
    if(is_anonymous_name(decl->getDeclName())) {
        if(llvm::isa<clang::NamespaceDecl>(decl)) {
            return "(anonymous namespace)";
        }

        if(auto* record = llvm::dyn_cast<clang::RecordDecl>(decl)) {
            if(record->isLambda()) {
                return "(lambda)";
            }
            return std::format("(anonymous {})", record->getKindName());
        }

        if(llvm::isa<clang::EnumDecl>(decl)) {
            return "(anonymous enum)";
        }

        return "(anonymous)";
    }

    /// Print nested name qualifier if it was written in the source code.
    if(auto qualifier = qualifier_loc(*decl).getNestedNameSpecifier()) {
        qualifier.print(os, policy);
    }

    /// Print the name itself — a conversion's as declared, see bare_name.
    if(llvm::isa<clang::CXXConversionDecl>(decl)) {
        os << bare_name(decl, options);
    } else {
        decl->getDeclName().print(os, policy);
    }

    /// Print template arguments.
    os << template_args(*decl);

    return name;
}

auto summarize(const clang::Expr* expr) -> std::string {
    assert(expr);
    using namespace clang;

    struct Namer : ConstStmtVisitor<Namer, std::string> {
        std::string Visit(const Expr* E) {
            if(E == nullptr)
                return "";
            return ConstStmtVisitor::Visit(E->IgnoreImplicit());
        }

        // Any sort of decl reference, we just use the unqualified name.
        std::string VisitMemberExpr(const MemberExpr* E) {
            return simple_name(E->getMemberDecl()->getDeclName()).str();
        }

        std::string VisitDeclRefExpr(const DeclRefExpr* E) {
            return simple_name(E->getFoundDecl()->getDeclName()).str();
        }

        std::string VisitCallExpr(const CallExpr* E) {
            std::string Result = Visit(E->getCallee());
            if(!Result.empty()) {
                bool written =
                    E->getNumArgs() != 0 && !llvm::isa<clang::CXXDefaultArgExpr>(E->getArg(0));
                Result += written ? "(...)" : "()";
            }
            return Result;
        }

        std::string VisitCXXDependentScopeMemberExpr(const CXXDependentScopeMemberExpr* E) {
            return simple_name(E->getMember()).str();
        }

        std::string VisitDependentScopeDeclRefExpr(const DependentScopeDeclRefExpr* E) {
            return simple_name(E->getDeclName()).str();
        }

        std::string VisitCXXFunctionalCastExpr(const CXXFunctionalCastExpr* E) {
            return type_identifier(E->getType()).str();
        }

        std::string VisitCXXTemporaryObjectExpr(const CXXTemporaryObjectExpr* E) {
            return type_identifier(E->getType()).str();
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
        std::string VisitCXXNullPtrLiteralExpr(const CXXNullPtrLiteralExpr* E) {
            return "nullptr";
        }

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
            } else {
                llvm::raw_string_ostream OS(Result);
                if(E->getLength() > 10) {
                    llvm::printEscapedString(E->getString().take_front(7), OS);
                    Result += "...";
                } else {
                    llvm::printEscapedString(E->getString(), OS);
                }
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

    return Namer{}.Visit(expr);
}

auto printing_policy(clang::ASTContext& context, const Options& options) -> clang::PrintingPolicy {
    clang::PrintingPolicy policy = context.getPrintingPolicy();
    policy.SuppressScope = options.suppress_scope;
    policy.SuppressUnwrittenScope = options.suppress_unwritten_scope;
    policy.TerseOutput = options.terse;
    policy.PolishForDeclaration = options.polish_for_declaration;
    policy.ConstantsAsWritten = options.constants_as_written;
    policy.SuppressTemplateArgsInCXXConstructors = options.suppress_ctor_template_args;
    policy.CleanUglifiedParameters = options.clean_uglified;
    policy.AnonymousTagNameStyle =
        std::to_underlying(clang::PrintingPolicy::AnonymousTagMode::Plain);
    // As clang's diagnostics do: C's `_Bool` reads `bool` where a macro
    // (<stdbool.h>) spells it so.
    if(!policy.Bool) {
        policy.Bool = context.getBoolName()->hasMacroDefinition();
    }
    return policy;
}

auto restored_scope(clang::QualType type, const Options& options) -> clang::NestedNameSpecifier {
    /// Complex cases (pointers/references, cv-qualifiers) are not attempted.
    if(!options.suppress_scope || type.isNull() || type.hasQualifiers()) {
        return std::nullopt;
    }
    if(auto* AT = llvm::dyn_cast<clang::AutoType>(type.getTypePtr());
       AT && AT->isDeduced() && !AT->getDeducedType().isNull()) {
        type = AT->getDeducedType();
    }

    bool scope_suppressed = false;
    if(auto* tag = llvm::dyn_cast<clang::TagType>(type.getTypePtr())) {
        scope_suppressed = tag->isCanonicalUnqualified();
    } else if(auto* TST = llvm::dyn_cast<clang::TemplateSpecializationType>(type.getTypePtr())) {
        scope_suppressed = !TST->getTemplateName().getAsDependentTemplateName();
    }
    if(!scope_suppressed || type.hasQualifiers()) {
        return std::nullopt;
    }
    return type->getPrefix();
}

auto type(clang::ASTContext& context,
          clang::QualType type,
          const Options& options,
          llvm::StringRef declarator) -> Type {
    clang::PrintingPolicy policy = printing_policy(context, options);
    if(options.readable && !type.isNull()) {
        type = options.readable->readable(type);
    }

    if(options.resolve_decltype) {
        /// TypePrinter doesn't resolve decltypes, so resolve them here.
        /// FIXME: This doesn't handle composite types that contain a decltype in
        /// them. We should rather have a printing policy for that.
        while(!type.isNull() && type->isDecltypeType()) {
            type =
                context.getQualifiedType(type->castAs<clang::DecltypeType>()->getUnderlyingType(),
                                         type.getLocalQualifiers());
        }
    }

    Type result;
    llvm::raw_string_ostream os(result.text);

    if(auto scope = restored_scope(type, options)) {
        scope.print(os, policy);
    }
    type.print(os, policy, declarator);

    if(!type.isNull() && options.show_aka) {
        /// A placeholder prints as the type deduced for it, and only that
        /// type's own sugar is worth an aka: desugaring the placeholder would
        /// spell a class template argument deduction's result again, only
        /// qualified.
        auto sugared = type;
        while(const auto* deduced = llvm::dyn_cast<clang::DeducedType>(sugared.getTypePtr())) {
            /// A placeholder deduced as dependent has no type yet.
            if(deduced->getDeducedType().isNull()) {
                break;
            }
            sugared =
                context.getQualifiedType(deduced->getDeducedType(), sugared.getLocalQualifiers());
        }
        if(auto desugared = desugar(context, sugared)) {
            result.aka = desugared->getAsString(policy);
        }
    }
    return result;
}

auto maybe_desugar(clang::ASTContext& context, clang::QualType type) -> clang::QualType {
    /// Prefer desugared type for name that aliases the template parameters.
    /// This can prevent things like printing opaque `: type` when accessing std
    /// containers.
    if(is_sugared_template_parameter(type)) {
        return desugar(context, type).value_or(type);
    }

    /// Prefer desugared type for `decltype(expr)` specifiers.
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

namespace {

/// Non-negative numbers are printed using min digits:
///   0     => 0x0
///   100   => 0x64
/// Negative numbers are sign-extended to 32/64 bits:
///   -2    => 0xfffffffe
///   -2^32 => 0xffffffff00000000
auto print_hex(const llvm::APSInt& value) -> llvm::FormattedNumber {
    assert(value.getSignificantBits() <= 64 && "Can't print more than 64 bits.");
    std::uint64_t bits =
        value.getBitWidth() > 64 ? value.trunc(64).getZExtValue() : value.getZExtValue();
    if(value.isNegative() && value.getSignificantBits() <= 32) {
        return llvm::format_hex(static_cast<std::uint32_t>(bits), 0);
    }
    return llvm::format_hex(bits, 0);
}

/// A string as a literal of `element` characters writes it, its quotes,
/// prefix and escapes included; cut short with `...` past `limit` characters.
auto quoted(const clang::ASTContext& context,
            clang::QualType element,
            llvm::ArrayRef<std::uint32_t> characters) -> std::string {
    constexpr std::size_t limit = 80;
    std::string text;
    if(element->isChar8Type()) {
        text = "u8";
    } else if(element->isChar16Type()) {
        text = "u";
    } else if(element->isChar32Type()) {
        text = "U";
    } else if(element->isWideCharType()) {
        text = "L";
    }
    text += '"';
    bool narrow = context.getTypeSize(element) == 8;
    for(auto [index, c]: llvm::enumerate(characters)) {
        if(index == limit) {
            text += "...";
            break;
        }
        switch(c) {
            case '"': text += R"(\")"; continue;
            case '\\': text += R"(\\)"; continue;
            case '\n': text += R"(\n)"; continue;
            case '\t': text += R"(\t)"; continue;
            default: break;
        }
        if(c < 0x20 || c == 0x7f) {
            text += R"(\x)";
            text += llvm::utohexstr(c, /*LowerCase=*/true, /*Width=*/2);
        } else if(narrow) {
            // UTF-8 code units pass through as the bytes they are.
            text += static_cast<char>(c);
        } else {
            std::array<char, 4> buffer;
            char* end = buffer.data();
            if(llvm::ConvertCodePointToUTF8(c, end)) {
                text.append(buffer.data(), end);
            }
        }
    }
    text += '"';
    return text;
}

/// The value of a character array or of a pointer to a string literal's
/// start as the string it holds, up to its terminator; nullopt for any
/// other value, and for an array no terminator ends.
auto string_value(const clang::ASTContext& context,
                  const clang::APValue& value,
                  clang::QualType type) -> std::optional<std::string> {
    if(value.isArray()) {
        const auto* array = context.getAsArrayType(type);
        if(!array || !array->getElementType()->isAnyCharacterType()) {
            return std::nullopt;
        }
        llvm::SmallVector<std::uint32_t> characters;
        for(unsigned index = 0; index < value.getArraySize(); index += 1) {
            const auto& element = index < value.getArrayInitializedElts()
                                      ? value.getArrayInitializedElt(index)
                                      : value.getArrayFiller();
            if(!element.isInt()) {
                return std::nullopt;
            }
            auto c = element.getInt().getZExtValue();
            if(c == 0) {
                return quoted(context, array->getElementType(), characters);
            }
            characters.push_back(static_cast<std::uint32_t>(c));
        }
        return std::nullopt;
    }

    if(value.isLValue() && !value.isLValueOnePastTheEnd() && value.getLValueOffset().isZero()) {
        const auto* literal = llvm::dyn_cast_if_present<clang::StringLiteral>(
            value.getLValueBase().dyn_cast<const clang::Expr*>());
        if(!literal || !type->isPointerType()) {
            return std::nullopt;
        }
        llvm::SmallVector<std::uint32_t> characters;
        for(unsigned index = 0; index < literal->getLength(); index += 1) {
            characters.push_back(literal->getCodeUnit(index));
        }
        return quoted(context, type->getPointeeType().getUnqualifiedType(), characters);
    }
    return std::nullopt;
}

/// The shortest text that reads back as the same value: `3.14` rather than
/// `3.1400000000000001`, the digits a double holds.
auto float_text(const llvm::APFloat& value) -> std::string {
    llvm::SmallString<32> text;
    if(value.isFinite()) {
        for(unsigned precision = 1; precision < 40; precision += 1) {
            text.clear();
            value.toString(text, precision);
            llvm::APFloat read(value.getSemantics());
            llvm::cantFail(read.convertFromString(text, llvm::APFloat::rmNearestTiesToEven));
            if(read.bitwiseIsEqual(value)) {
                return text.str().str();
            }
        }
    }
    text.clear();
    value.toString(text);
    return text.str().str();
}

}  // namespace

auto expr_value(const clang::ASTContext& context, const clang::Expr* expr)
    -> std::optional<std::string> {
    /// InitListExpr has two forms, syntactic and semantic. They are the same
    /// thing (refer to a same AST node) in most cases. When they are different,
    /// RAV returns the syntactic form, and we should feed the semantic form to
    /// EvaluateAsRValue.
    if(const auto* init_list = llvm::dyn_cast<clang::InitListExpr>(expr)) {
        if(!init_list->isSemanticForm()) {
            expr = init_list->getSemanticForm();
        }
    }

    /// Evaluating [[foo]]() as "&foo" isn't useful, and prevents us walking up
    /// to the enclosing call. Evaluating an expression of void type doesn't
    /// produce a meaningful result.
    clang::QualType type = expr->getType();
    if(type.isNull() || type->isFunctionType() || type->isFunctionPointerType() ||
       type->isFunctionReferenceType() || type->isVoidType()) {
        return std::nullopt;
    }

    clang::Expr::EvalResult constant;
    /// Attempt to evaluate. If the expr is dependent, evaluation crashes!
    if(expr->isValueDependent() || !expr->EvaluateAsRValue(constant, context) ||
       /// Disable printing for record-types, as they are usually confusing and
       /// might make clang crash while printing the expressions.
       constant.Val.isStruct() || constant.Val.isUnion()) {
        return std::nullopt;
    }

    if(auto text = string_value(context, constant.Val, type)) {
        return text;
    }
    if(constant.Val.isFloat()) {
        return float_text(constant.Val.getFloat());
    }
    /// As clang prints a complex integer, `0+6i`.
    if(constant.Val.isComplexFloat()) {
        return float_text(constant.Val.getComplexFloatReal()) + "+" +
               float_text(constant.Val.getComplexFloatImag()) + "i";
    }

    /// Show enums symbolically, not numerically like APValue::printPretty().
    if(type->isEnumeralType() && constant.Val.isInt()) {
        for(const clang::EnumConstantDecl* enumerator:
            type->castAs<clang::EnumType>()->getDecl()->enumerators()) {
            if(llvm::APSInt::isSameValue(enumerator->getInitVal(), constant.Val.getInt())) {
                if(constant.Val.getInt().getSignificantBits() > 64) {
                    return enumerator->getNameAsString();
                }
                return llvm::formatv("{0} ({1})",
                                     enumerator->getNameAsString(),
                                     print_hex(constant.Val.getInt()))
                    .str();
            }
        }
    }

    /// Show hex value of integers if they're at least 10 (or negative!).
    if(type->isIntegralOrEnumerationType() && constant.Val.isInt() &&
       constant.Val.getInt().getSignificantBits() <= 64 && constant.Val.getInt().uge(10)) {
        return llvm::formatv("{0} ({1})",
                             constant.Val.getAsString(context, type),
                             print_hex(constant.Val.getInt()))
            .str();
    }

    /// Arrays show their first elements only, as in diagnostics.
    clang::PrintingPolicy policy = context.getPrintingPolicy();
    policy.EntireContentsOfLargeArray = false;
    std::string value;
    llvm::raw_string_ostream os(value);
    constant.Val.printPretty(os, policy, type, &context);
    return value;
}

namespace {

/// An `#embed` or a string literal is a single token of any length.
auto printed_length(const clang::Expr& expr, const clang::PrintingPolicy& policy) -> std::size_t {
    std::string text;
    llvm::raw_string_ostream os(text);
    expr.printPretty(os, nullptr, policy);
    return text.size();
}

/// Default argument might exist but be unavailable, in the case of unparsed
/// arguments for example. This function returns the default argument if it is
/// available.
auto default_arg(const clang::ParmVarDecl* param) -> const clang::Expr* {
    /// Default argument can be unparsed or uninstantiated. For the former we
    /// can't do much, as token information is only stored in Sema and not
    /// attached to the AST node. For the latter though, it is safe to proceed as
    /// the expression is still valid.
    if(!param->hasDefaultArg() || param->hasUnparsedDefaultArg()) {
        return nullptr;
    }
    return param->hasUninstantiatedDefaultArg() ? param->getUninstantiatedDefaultArg()
                                                : param->getDefaultArg();
}

auto param_type(const clang::TemplateTypeParmDecl& param) -> Type {
    Type result;
    result.text = param.wasDeclaredWithTypename() ? "typename" : "class";
    if(param.isParameterPack()) {
        result.text += "...";
    }
    return result;
}

auto param_type(const clang::NonTypeTemplateParmDecl& param, const Options& options) -> Type {
    auto result = type(param.getASTContext(), param.getType(), options);
    if(param.isParameterPack()) {
        result.text += "...";
        if(result.aka) {
            *result.aka += "...";
        }
    }
    return result;
}

auto param_type(const clang::TemplateTemplateParmDecl& param, const Options& options) -> Type {
    Type result;
    llvm::raw_string_ostream os(result.text);
    os << "template <";
    llvm::StringRef sep = "";
    for(const clang::Decl* decl: *param.getTemplateParameters()) {
        os << sep;
        sep = ", ";
        if(const auto* type_param = llvm::dyn_cast<clang::TemplateTypeParmDecl>(decl)) {
            os << param_type(*type_param).text;
        } else if(const auto* non_type_param =
                      llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(decl)) {
            os << param_type(*non_type_param, options).text;
        } else if(const auto* template_param =
                      llvm::dyn_cast<clang::TemplateTemplateParmDecl>(decl)) {
            os << param_type(*template_param, options).text;
        }
    }
    /// FIXME: TemplateTemplateParameter doesn't store the info on whether this
    /// param was a "typename" or "class".
    os << "> class";
    return result;
}

/// `expr` as the source writes it, on one line.
auto as_written(clang::ASTContext& context, const clang::Expr* expr, const Options& options)
    -> std::string {
    auto range = clang::Lexer::makeFileCharRange(
        clang::CharSourceRange::getTokenRange(expr->getSourceRange()),
        context.getSourceManager(),
        context.getLangOpts());
    std::string text;
    if(range.isValid()) {
        auto source =
            clang::Lexer::getSourceText(range, context.getSourceManager(), context.getLangOpts());
        for(auto line: llvm::split(source, '\n')) {
            line = line.trim();
            if(line.empty()) {
                continue;
            }
            if(!text.empty()) {
                text += ' ';
            }
            text += line;
        }
        return text;
    }

    // Written in a macro's body: the source holds the macro's text, not
    // this declaration's.
    auto policy = printing_policy(context, options);
    policy.ConstantsAsWritten = true;
    llvm::raw_string_ostream os(text);
    expr->printPretty(os, nullptr, policy, 0, "\n", &context);
    return text;
}

}  // namespace

auto default_argument(const clang::ParmVarDecl* param, const Options& options)
    -> std::optional<std::string> {
    const clang::Expr* arg = default_arg(param);
    if(!arg || arg->containsErrors()) {
        return std::nullopt;
    }
    return as_written(param->getASTContext(), arg, options);
}

auto method_qualifiers(const clang::CXXMethodDecl& method) -> std::string {
    std::string text;
    if(auto qualifiers = method.getMethodQualifiers(); !qualifiers.empty()) {
        text += " " + qualifiers.getAsString();
    }
    if(method.getRefQualifier() == clang::RQ_LValue) {
        text += " &";
    } else if(method.getRefQualifier() == clang::RQ_RValue) {
        text += " &&";
    }
    return text;
}

auto parameter_name(const clang::FunctionDecl& function, unsigned index) -> llvm::StringRef {
    auto named = decls::named_parameters(function, index);
    return named.empty() ? llvm::StringRef() : named.front()->getIdentifier()->deuglifiedName();
}

auto param(const clang::ParmVarDecl* param, const Options& options) -> Param {
    assert(param);
    Param result;
    result.type = type(param->getASTContext(), param->getType(), options);
    auto named = decls::named_parameters(*param);
    if(!named.empty()) {
        result.name = named.front()->getName().str();
        for(const auto* declaration: named) {
            result.written.push_back(declaration->getName().str());
        }
    }
    result.default_value = default_argument(param, options);
    result.declaration = parameter(*param, result.name.value_or(""), options);
    return result;
}

auto parameter(const clang::ParmVarDecl& param, llvm::StringRef name, const Options& options)
    -> std::string {
    // Implicit special members write no type.
    auto written =
        param.getTypeSourceInfo() ? param.getTypeSourceInfo()->getType() : param.getType();
    std::string declarator;
    if(const auto* expansion = written->getAs<clang::PackExpansionType>()) {
        written = expansion->getPattern();
        declarator = "...";
    }
    declarator += name;
    std::string text;
    /// A parameter of function type, `int callback(int)`: clang would put
    /// the name in parentheses, `int (callback)(int)`.
    if(const auto* function = written->getAs<clang::FunctionProtoType>();
       function && !name.empty()) {
        auto& context = param.getASTContext();
        text = type(context, function->getReturnType(), options).text + " " + declarator + "(";
        llvm::raw_string_ostream os(text);
        llvm::interleave(
            function->getParamTypes(),
            os,
            [&](clang::QualType param_type) { os << type(context, param_type, options).text; },
            ", ");
        os << (function->isVariadic() ? (function->getNumParams() ? ", ...)" : "...)") : ")");
    } else {
        text = type(param.getASTContext(), written, options, declarator).text;
    }
    if(auto value = default_argument(&param, options)) {
        text += " = " + *value;
    }
    return text;
}

auto field(const clang::FieldDecl& field, const Options& options) -> std::string {
    auto& context = field.getASTContext();
    auto text = field.isMutable() ? std::string("mutable ") : std::string();
    text += type(context, field.getType(), options, field.getName()).text;
    if(const auto* width = field.getBitWidth()) {
        text += " : " + as_written(context, width, options);
    }
    if(const auto* init = field.getInClassInitializer()) {
        text += field.getInClassInitStyle() == clang::ICIS_ListInit ? " " : " = ";
        text += as_written(context, init, options);
    }
    return text;
}

auto template_param_type(const clang::NamedDecl* param, const Options& options) -> Type {
    assert(param);
    if(const auto* type_param = llvm::dyn_cast<clang::TemplateTypeParmDecl>(param)) {
        return param_type(*type_param);
    }
    if(const auto* non_type_param = llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(param)) {
        return param_type(*non_type_param, options);
    }
    if(const auto* template_param = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(param)) {
        return param_type(*template_param, options);
    }
    return {};
}

namespace {

using TokenCount = llvm::function_ref<std::size_t(clang::SourceRange)>;

/// Initializers might be huge and result in lots of memory allocations in
/// some catastrophic cases. Such long lists are not useful in hover cards
/// anyway, and neither is one that does not compile: clang prints the parts
/// it recovered as `<recovery-expr>`.
bool hides_initializer(const clang::Expr* init,
                       const clang::PrintingPolicy& policy,
                       TokenCount token_count) {
    return init && (init->containsErrors() || token_count(init->getSourceRange()) > 200 ||
                    printed_length(*init, policy) > 500);
}

auto initializer(const clang::Decl& member) -> const clang::Expr* {
    if(const auto* field = llvm::dyn_cast<clang::FieldDecl>(&member)) {
        return field->getInClassInitializer();
    }
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(&member)) {
        return var->getInit();
    }
    if(const auto* var_template = llvm::dyn_cast<clang::VarTemplateDecl>(&member)) {
        return var_template->getTemplatedDecl()->getInit();
    }
    return nullptr;
}

/// Clang's terse rendering of a tag declaration — template prefix,
/// attributes, bases — without the empty body it gives a definition: "{}"
/// for a C++ class, "{\n}" for an enum or a C struct.
auto tag_head(const clang::Decl& decl, const clang::PrintingPolicy& policy) -> std::string {
    std::string text;
    llvm::raw_string_ostream os(text);
    decl.print(os, policy);
    llvm::StringRef head = text;
    if(head.consume_back("{}") || head.consume_back("{\n}")) {
        head = head.rtrim();
    }
    return head.str();
}

/// Whether the tag is declared on its own, as a nested type or an
/// anonymous struct or union, not inside another member's declaration as in
/// `struct item* first;` or `char raw[sizeof(struct { ... })];`. An
/// instantiation leaves that to its pattern.
bool is_free_standing(const clang::TagDecl& tag) {
    const clang::TagDecl* written = &tag;
    if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&tag);
       record && record->getInstantiatedFromMemberClass()) {
        written = record->getInstantiatedFromMemberClass();
    } else if(const auto* enum_decl = llvm::dyn_cast<clang::EnumDecl>(&tag);
              enum_decl && enum_decl->getInstantiatedFromMemberEnum()) {
        written = enum_decl->getInstantiatedFromMemberEnum();
    }
    return written->isFreeStanding();
}

/// Whether a class summary lists the member: its data and the types it
/// declares. A type declared twice is listed once, and a specialization of
/// a member template is left to the template.
bool is_listed(const clang::Decl& member) {
    if(member.isImplicit() || !member.isFirstDecl() ||
       llvm::isa<clang::ClassTemplateSpecializationDecl, clang::VarTemplateSpecializationDecl>(
           member)) {
        return false;
    }
    /// A reserved name is the implementation's own (`_M_impl`, `__base`).
    if(const auto* named = llvm::dyn_cast<clang::NamedDecl>(&member);
       named && clang::isReservedInAllContexts(named->isReserved(member.getLangOpts()))) {
        return false;
    }
    return llvm::isa<clang::FieldDecl,
                     clang::VarDecl,
                     clang::VarTemplateDecl,
                     clang::TypedefNameDecl,
                     clang::TypeAliasTemplateDecl,
                     clang::TagDecl,
                     clang::ClassTemplateDecl>(member);
}

/// Whether a field or a typedef is declared along with the tag, as `a` and
/// `b` are in `struct { ... } a, *b;`: its type is spelled from the tag on,
/// and is the tag behind pointers and arrays. Locations tell where the AST
/// does not: an instantiation drops the type's tie to the declaration that
/// defines it.
bool declared_with(const clang::Decl& member, const clang::TagDecl& tag) {
    clang::SourceLocation type_start;
    clang::QualType type;
    if(const auto* field = llvm::dyn_cast<clang::FieldDecl>(&member);
       field && !field->isImplicit()) {
        type_start = field->getTypeSpecStartLoc();
        type = field->getType();
    } else if(const auto* typedef_decl = llvm::dyn_cast<clang::TypedefDecl>(&member)) {
        type_start = typedef_decl->getTypeSourceInfo()->getTypeLoc().getBeginLoc();
        type = typedef_decl->getUnderlyingType();
    } else {
        return false;
    }
    return type_start == tag.getBeginLoc() && types::unwrap(type)->getAs<clang::TagType>();
}

/// Prints the body of a tag definition as a summary: the enumerators of an
/// enum, the data members and member types of a class. Member functions are
/// left out: code completion lists them with their documentation, and here
/// they would crowd out the data that makes up the type.
struct MemberPrinter {
    llvm::raw_ostream& os;
    const clang::PrintingPolicy& policy;
    const Options& options;
    TokenCount token_count;

    /// How many more members to print, those of nested unnamed tags included.
    std::uint32_t budget;

    /// Whether one more member fits; marks the cut when it does not.
    bool fits() {
        if(budget == 0) {
            os << "\n// ...";
            return false;
        }
        budget -= 1;
        return true;
    }

    void body(const clang::TagDecl& tag) {
        os << " {";
        if(const auto* enum_decl = llvm::dyn_cast<clang::EnumDecl>(&tag)) {
            enumerators(*enum_decl);
        } else if(tag.isCompleteDefinition()) {
            members(llvm::cast<clang::RecordDecl>(tag));
        } else {
            /// An instantiation defines a member class only once it is used.
            os << "\n// ...";
        }
        os << "\n}";
    }

    void enumerators(const clang::EnumDecl& decl) {
        llvm::StringRef separator;
        for(const clang::EnumConstantDecl* enumerator: decl.enumerators()) {
            os << separator;
            separator = ",";
            if(!fits()) {
                return;
            }
            os << '\n' << enumerator->getName();
            if(const clang::Expr* init = enumerator->getInitExpr();
               init && !hides_initializer(init, policy, token_count)) {
                os << " = ";
                init->printPretty(os, nullptr, policy);
            } else if(!enumerator->getType()->isDependentType()) {
                os << " = " << llvm::toString(enumerator->getInitVal(), 10);
            }
        }
    }

    void members(const clang::RecordDecl& record) {
        auto access = record.isClass() ? clang::AS_private : clang::AS_public;
        const auto* cxx = llvm::dyn_cast<clang::CXXRecordDecl>(&record);
        bool instantiation = cxx && cxx->getTemplateInstantiationPattern();
        auto decls = record.decls();
        for(auto it = decls.begin(); it != decls.end(); ++it) {
            const clang::Decl* member = *it;
            if(!is_listed(*member)) {
                continue;
            }

            const auto* tag = llvm::dyn_cast<clang::TagDecl>(member);
            llvm::SmallVector<const clang::Decl*> declarators;
            auto last = it;
            if(tag) {
                /// A tag mentioned or defined inside a declarator, as in
                /// `struct { ... } a[sizeof(struct B)], *b;`, comes between.
                for(auto next = std::next(it); next != decls.end(); ++next) {
                    if(declared_with(**next, *tag)) {
                        declarators.push_back(*next);
                        last = next;
                    } else if(!llvm::isa<clang::TagDecl>(*next)) {
                        break;
                    }
                }
                if(declarators.empty() && !is_free_standing(*tag)) {
                    continue;
                }
            }
            if(!fits()) {
                return;
            }

            /// C has no access control: its members have AS_none.
            if(member->getAccess() != access && member->getAccess() != clang::AS_none) {
                access = member->getAccess();
                os << '\n' << clang::getAccessSpelling(access) << ':';
            }
            os << '\n';

            if(tag) {
                if(!declarators.empty()) {
                    specifiers(*declarators.front());
                }
                /// A named type has its own hover card; an unnamed one shows
                /// its members only here.
                os << tag_head(*tag, policy);
                if(!tag->getDeclName()) {
                    body(*tag);
                }
                auto declarator_policy = policy;
                declarator_policy.SuppressSpecifiers = true;
                llvm::StringRef separator = " ";
                for(const clang::Decl* declarator: declarators) {
                    os << separator;
                    separator = ", ";
                    print(*declarator, declarator_policy);
                }
                it = last;
            } else if(llvm::isa<clang::ClassTemplateDecl>(member)) {
                os << tag_head(*member, policy);
            } else if(instantiation) {
                instantiated(*member);
            } else {
                print(*member, policy);
            }
            os << ';';
        }
    }

    /// A member of a class template's instantiation: its types as a reader
    /// knows them, rather than as the template writes them in terms of its
    /// own parameters and helpers (`typedef typename _Base::pointer
    /// pointer`).
    void instantiated(const clang::Decl& member) {
        auto& context = member.getASTContext();
        if(const auto* field = llvm::dyn_cast<clang::FieldDecl>(&member)) {
            os << display::field(*field, options);
        } else if(const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(&member)) {
            auto target = type(context, member_type(alias->getUnderlyingType()), options).text;
            if(llvm::isa<clang::TypeAliasDecl>(alias)) {
                os << "using " << alias->getName() << " = " << target;
            } else {
                os << "typedef "
                   << type(context,
                           member_type(alias->getUnderlyingType()),
                           options,
                           alias->getName())
                          .text;
            }
        } else {
            print(member, policy);
        }
    }

    /// The type a member type of an instantiation names, through the
    /// aliases of its class and its bases.
    static auto member_type(clang::QualType type) -> clang::QualType {
        while(const auto* alias = type->getAs<clang::TypedefType>()) {
            if(!alias->getDecl()->getDeclContext()->isRecord()) {
                break;
            }
            type = type.getSingleStepDesugaredType(alias->getDecl()->getASTContext());
        }
        return type;
    }

    /// The specifiers the declarators in `const struct { ... } a, *b;`
    /// share; each of them prints without.
    void specifiers(const clang::Decl& declarator) {
        clang::QualType type;
        if(const auto* typedef_decl = llvm::dyn_cast<clang::TypedefDecl>(&declarator)) {
            os << "typedef ";
            type = typedef_decl->getUnderlyingType();
        } else {
            const auto& field = llvm::cast<clang::FieldDecl>(declarator);
            if(field.isMutable()) {
                os << "mutable ";
            }
            type = field.getType();
        }
        types::unwrap(type).getQualifiers().print(os, policy, /*appendSpaceIfNonEmpty=*/true);
    }

    void print(const clang::Decl& member, clang::PrintingPolicy member_policy) {
        if(hides_initializer(initializer(member), member_policy, token_count)) {
            member_policy.SuppressInitializers = true;
        }
        if(const auto* var = llvm::dyn_cast<clang::VarDecl>(&member);
           var && var->isInlineSpecified()) {
            os << "inline ";
        }
        member.print(os, member_policy);
        if(const auto* init = template_initializer(member);
           init && !member_policy.SuppressInitializers &&
           !hides_initializer(init, member_policy, token_count)) {
            os << " = ";
            init->printPretty(os, nullptr, member_policy);
        }
    }

    /// The initializer of a class template's static data member or member
    /// variable template, for an instantiation that has none of its own
    /// yet: clang instantiates it only where it is used. One that depends
    /// on the template's arguments says nothing until then.
    static auto template_initializer(const clang::Decl& member) -> const clang::Expr* {
        const auto* var = llvm::dyn_cast<clang::VarDecl>(&member);
        const clang::VarDecl* pattern = var ? var->getInstantiatedFromStaticDataMember() : nullptr;
        if(const auto* var_template = llvm::dyn_cast<clang::VarTemplateDecl>(&member)) {
            var = var_template->getTemplatedDecl();
            if(const auto* from = var_template->getInstantiatedFromMemberTemplate()) {
                pattern = from->getTemplatedDecl();
            }
        }
        if(!pattern || var->getInit()) {
            return nullptr;
        }
        const auto* init = pattern->getInit();
        return init && !init->isValueDependent() ? init : nullptr;
    }
};

/// Clang prints a structured binding declaration without its bindings.
void print_decomposition(const clang::DecompositionDecl& decl,
                         const clang::PrintingPolicy& policy,
                         llvm::raw_ostream& os) {
    if(decl.getStorageClass() != clang::SC_None) {
        os << clang::VarDecl::getStorageClassSpecifierString(decl.getStorageClass()) << ' ';
    }
    if(decl.getTSCSpec() == clang::TSCS_thread_local) {
        os << "thread_local ";
    }

    std::string bindings = "[";
    llvm::raw_string_ostream names(bindings);
    llvm::interleaveComma(decl.bindings(), names, [&](const clang::BindingDecl* binding) {
        names << (binding->isParameterPack() ? "..." : "") << binding->getName();
    });
    bindings += ']';
    decl.getTypeSourceInfo()->getType().print(os, policy, bindings);

    /// A range-for's bindings are initialized from the range's iterator.
    const clang::Expr* init = decl.getInit();
    if(!init || policy.SuppressInitializers || decl.isCXXForRangeDecl()) {
        return;
    }
    bool call = decl.getInitStyle() == clang::VarDecl::CallInit;
    if(decl.getInitStyle() == clang::VarDecl::CInit) {
        os << " = ";
    } else if(call) {
        os << '(';
    }
    init->printPretty(os, nullptr, policy);
    if(call) {
        os << ')';
    }
}

}  // namespace

namespace {

/// The head of a class template's instantiation: its arguments as a reader
/// names them (specialization_args), and its bases but those the library
/// keeps to itself (`_Vector_base`).
auto instantiation_head(const clang::ClassTemplateSpecializationDecl& spec, const Options& options)
    -> std::string {
    std::string text = "template <> ";
    text += spec.getKindName();
    text += ' ';
    text += spec.getName();
    text += specialization_args(spec, options);
    if(spec.hasAttr<clang::FinalAttr>()) {
        text += " final";
    }
    llvm::StringRef separator = " : ";
    for(const auto& base: spec.bases()) {
        const auto* base_decl = base.getType()->getAsCXXRecordDecl();
        if(base_decl && options.readable && options.readable->internal(base_decl)) {
            continue;
        }
        text += separator;
        separator = ", ";
        if(base.isVirtual()) {
            text += "virtual ";
        }
        if(auto access = base.getAccessSpecifierAsWritten(); access != clang::AS_none) {
            text += clang::getAccessSpelling(access);
            text += ' ';
        }
        text += type(spec.getASTContext(), base.getType(), options).text;
    }
    return text;
}

/// Whether the alias names a tag declared with it and named by it alone, as
/// in `typedef struct { ... } Point;`.
bool declares_tag(const clang::TypedefNameDecl& alias) {
    const auto* tag = alias.getUnderlyingType()->getAsTagDecl();
    return tag && tag->getTypedefNameForAnonDecl() == &alias && tag->getDefinition();
}

/// Clang's printer writes a function under the qualifier its definition is
/// written with, and a pack among a specialization's arguments as a nested
/// list (`vector<int>::emplace_back<<int, int>>`, which then formats as a
/// shift): the name alone, its arguments flattened.
void print_name(std::string& text,
                const clang::FunctionDecl& function,
                const clang::PrintingPolicy& policy) {
    std::string name;
    llvm::raw_string_ostream name_os(name);
    function.getNameInfo().printName(name_os, policy);

    std::string printed;
    llvm::raw_string_ostream printed_os(printed);
    function.getQualifier().print(printed_os, policy);
    printed_os << name;

    const auto* args = function.getTemplateSpecializationArgs();
    if(args && !function.getTemplateSpecializationArgsAsWritten()) {
        printed_os << '<';
        llvm::interleave(
            args->asArray(),
            printed_os,
            [&](const clang::TemplateArgument& arg) {
                arg.print(policy, printed_os, /*IncludeType=*/true);
            },
            ", ");
        printed_os << '>';
        clang::printTemplateArgumentList(name_os, args->asArray(), policy);
    }

    if(printed != name) {
        if(auto at = text.find(printed); at != std::string::npos) {
            text.replace(at, printed.size(), name);
        }
    }
}

}  // namespace

auto definition(const clang::Decl* decl,
                const Options& options,
                llvm::function_ref<std::size_t(clang::SourceRange)> token_count) -> std::string {
    assert(decl);
    auto& context = decl->getASTContext();
    clang::PrintingPolicy policy = printing_policy(context, options);
    EnclosingScopes scopes(decl->getDeclContext());
    policy.Callbacks = &scopes;
    const auto* var = llvm::dyn_cast<clang::VarDecl>(decl);
    if(var && hides_initializer(var->getInit(), policy, token_count)) {
        policy.SuppressInitializers = true;
    }

    std::string definition;
    llvm::raw_string_ostream os(definition);
    MemberPrinter printer{
        .os = os,
        .policy = policy,
        .options = options,
        .token_count = token_count,
        .budget = options.max_members,
    };
    const auto* class_template = llvm::dyn_cast<clang::ClassTemplateDecl>(decl);
    const auto* tag =
        llvm::dyn_cast<clang::TagDecl>(class_template ? class_template->getTemplatedDecl() : decl);
    const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(decl);
    if(const auto* decomposition = llvm::dyn_cast<clang::DecompositionDecl>(decl)) {
        print_decomposition(*decomposition, policy, os);
    } else if(const auto* param = llvm::dyn_cast<clang::ParmVarDecl>(decl)) {
        if(param->isExplicitObjectParameter()) {
            os << "this ";
        }
        /// As written, as clang prints every other declaration.
        auto written = options;
        written.resolve_decltype = false;
        written.readable = nullptr;
        auto text = parameter(*param, param->getName(), written);
        /// A redeclaration inherits the default argument it does not write.
        if(auto value = default_argument(param, written);
           value && param->hasInheritedDefaultArg()) {
            text.resize(text.size() - value->size() - llvm::StringRef(" = ").size());
        }
        os << text;
    } else if(var && var->isInitCapture()) {
        /// Clang prints the capture as a variable of its deduced type,
        /// `auto` left in front.
        os << "auto " << (var->getType()->isReferenceType() ? "&" : "") << var->getName();
        if(var->getInit() && !policy.SuppressInitializers) {
            os << " = " << as_written(context, var->getInit(), options);
        }
    } else if(alias && declares_tag(*alias)) {
        /// Clang prints the tag under the alias's name, `typedef struct Point
        /// Point`, a struct C does not know.
        const auto* named = alias->getUnderlyingType()->getAsTagDecl()->getDefinition();
        if(llvm::isa<clang::TypeAliasDecl>(alias)) {
            os << "using " << alias->getName() << " = ";
        } else {
            os << "typedef ";
        }
        os << named->getKindName();
        printer.body(*named);
        if(llvm::isa<clang::TypedefDecl>(alias)) {
            os << ' ' << alias->getName();
        }
    } else if(!tag || !tag->getDefinition()) {
        /// Clang prints no `inline` of a variable.
        if(var && var->isInlineSpecified()) {
            os << "inline ";
        }
        decl->print(os, policy);
        if(const auto* function = decl->getAsFunction()) {
            print_name(definition, *function, policy);
        }
    } else {
        /// The head comes from the definition too: only a definition prints
        /// its bases. tag_head() relies on the terse body.
        assert(options.terse);
        tag = tag->getDefinition();
        const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(tag);
        if(class_template) {
            os << tag_head(*llvm::cast<clang::CXXRecordDecl>(tag)->getDescribedClassTemplate(),
                           policy);
        } else if(spec && !llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(spec) &&
                  spec->getSpecializationKind() != clang::TSK_ExplicitSpecialization) {
            os << instantiation_head(*spec, options);
        } else {
            os << tag_head(*tag, policy);
        }
        if(options.max_members > 0) {
            printer.body(*tag);
        }
    }

    /// The declarations of a private module fragment are module private,
    /// which clang prints as the `__module_private__` specifier of its own
    /// module maps: no C++20 code spells it.
    if(decl->isModulePrivate()) {
        constexpr llvm::StringLiteral specifier = "__module_private__ ";
        for(auto at = definition.find(specifier); at != std::string::npos;
            at = definition.find(specifier, at)) {
            definition.erase(at, specifier.size());
        }
    }
    return definition;
}

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Type& type) {
    os << type.text;
    if(type.aka) {
        os << " (aka " << *type.aka << ")";
    }
    return os;
}

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Param& param) {
    if(param.declaration) {
        os << *param.declaration;
    } else {
        if(param.type) {
            os << param.type->text;
        }
        if(param.name) {
            os << " " << *param.name;
        }
        if(param.default_value) {
            os << " = " << *param.default_value;
        }
    }
    if(param.type && param.type->aka) {
        os << " (aka " << *param.type->aka << ")";
    }
    return os;
}

auto ReadableTypes::written_scope(const clang::NamedDecl* decl) const
    -> clang::NestedNameSpecifier {
    // The namespaces the code names it through: an inline one is not written.
    clang::NestedNameSpecifier scope = std::nullopt;
    llvm::SmallVector<const clang::NamespaceDecl*> namespaces;
    for(const auto* parent = decl->getDeclContext(); parent; parent = parent->getParent()) {
        if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(parent); ns && !ns->isInline()) {
            namespaces.push_back(ns);
        }
    }
    for(const auto* ns: llvm::reverse(namespaces)) {
        scope = clang::NestedNameSpecifier(context, ns, scope);
    }
    return scope;
}

bool ReadableTypes::reserved(const clang::NamedDecl* decl) const {
    return decl && clang::isReservedInAllContexts(decl->isReserved(context.getLangOpts()));
}

bool ReadableTypes::internal(const clang::NamedDecl* decl) const {
    if(!reserved(decl)) {
        return false;
    }
    for(const auto* scope = decl->getDeclContext(); scope; scope = scope->getParent()) {
        if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(scope);
           ns && (ns->isStdNamespace() || reserved(ns))) {
            return true;
        }
    }
    return false;
}

namespace {

/// The typedefs the standard declares for specializations of its class
/// templates, by the template they name; one in a namespace of std is
/// written under it.
struct StandardTypedefs {
    llvm::StringLiteral pattern;
    llvm::StringLiteral names;
};

constexpr StandardTypedefs standard_typedefs[] = {
    {"basic_string",
     "string wstring u8string u16string u32string pmr::string pmr::wstring pmr::u8string "
     "pmr::u16string pmr::u32string"                                                                     },
    {"basic_string_view",          "string_view wstring_view u8string_view u16string_view u32string_view"},
    {"basic_ios",                  "ios wios"                                                            },
    {"basic_streambuf",            "streambuf wstreambuf"                                                },
    {"basic_istream",              "istream wistream"                                                    },
    {"basic_ostream",              "ostream wostream"                                                    },
    {"basic_iostream",             "iostream wiostream"                                                  },
    {"basic_stringbuf",            "stringbuf wstringbuf"                                                },
    {"basic_istringstream",        "istringstream wistringstream"                                        },
    {"basic_ostringstream",        "ostringstream wostringstream"                                        },
    {"basic_stringstream",         "stringstream wstringstream"                                          },
    {"basic_spanbuf",              "spanbuf wspanbuf"                                                    },
    {"basic_ispanstream",          "ispanstream wispanstream"                                            },
    {"basic_ospanstream",          "ospanstream wospanstream"                                            },
    {"basic_spanstream",           "spanstream wspanstream"                                              },
    {"basic_filebuf",              "filebuf wfilebuf"                                                    },
    {"basic_ifstream",             "ifstream wifstream"                                                  },
    {"basic_ofstream",             "ofstream wofstream"                                                  },
    {"basic_fstream",              "fstream wfstream"                                                    },
    {"basic_syncbuf",              "syncbuf wsyncbuf"                                                    },
    {"basic_osyncstream",          "osyncstream wosyncstream"                                            },
    {"fpos",                       "streampos wstreampos u8streampos u16streampos u32streampos"          },
    {"basic_regex",                "regex wregex"                                                        },
    {"sub_match",                  "csub_match wcsub_match ssub_match wssub_match"                       },
    {"match_results",
     "cmatch wcmatch smatch wsmatch pmr::cmatch pmr::wcmatch pmr::smatch pmr::wsmatch"                   },
    {"regex_iterator",             "cregex_iterator wcregex_iterator sregex_iterator wsregex_iterator"   },
    {"regex_token_iterator",
     "cregex_token_iterator wcregex_token_iterator sregex_token_iterator "
     "wsregex_token_iterator"                                                                            },
    {"basic_format_context",       "format_context wformat_context"                                      },
    {"basic_format_parse_context", "format_parse_context wformat_parse_context"                          },
    {"basic_format_args",          "format_args wformat_args"                                            },
    {"basic_stacktrace",           "stacktrace pmr::stacktrace"                                          },
    {"integral_constant",          "true_type false_type"                                                },
    {"ratio",
     "quecto ronto yocto zepto atto femto pico nano micro milli centi deci deca hecto kilo mega "
     "giga tera peta exa zetta yotta ronna quetta"                                                       },
    {"duration",
     "chrono::nanoseconds chrono::microseconds chrono::milliseconds chrono::seconds "
     "chrono::minutes chrono::hours chrono::days chrono::weeks chrono::months chrono::years"             },
    {"time_point",
     "chrono::sys_seconds chrono::sys_days chrono::local_seconds chrono::local_days "
     "chrono::utc_seconds chrono::tai_seconds chrono::gps_seconds filesystem::file_time_type"            },
    {"zoned_time",                 "chrono::zoned_seconds"                                               },
    {"atomic",
     "atomic_bool atomic_char atomic_schar atomic_uchar atomic_short atomic_ushort atomic_int "
     "atomic_uint atomic_long atomic_ulong atomic_llong atomic_ullong atomic_char8_t "
     "atomic_char16_t atomic_char32_t atomic_wchar_t atomic_int8_t atomic_uint8_t atomic_int16_t "
     "atomic_uint16_t atomic_int32_t atomic_uint32_t atomic_int64_t atomic_uint64_t "
     "atomic_int_least8_t atomic_uint_least8_t atomic_int_least16_t atomic_uint_least16_t "
     "atomic_int_least32_t atomic_uint_least32_t atomic_int_least64_t atomic_uint_least64_t "
     "atomic_int_fast8_t atomic_uint_fast8_t atomic_int_fast16_t atomic_uint_fast16_t "
     "atomic_int_fast32_t atomic_uint_fast32_t atomic_int_fast64_t atomic_uint_fast64_t "
     "atomic_intptr_t atomic_uintptr_t atomic_size_t atomic_ptrdiff_t atomic_intmax_t "
     "atomic_uintmax_t atomic_signed_lock_free atomic_unsigned_lock_free"                                },
    {"linear_congruential_engine", "minstd_rand0 minstd_rand"                                            },
    {"mersenne_twister_engine",    "mt19937 mt19937_64"                                                  },
    {"subtract_with_carry_engine", "ranlux24_base ranlux48_base"                                         },
    {"discard_block_engine",       "ranlux24 ranlux48"                                                   },
    {"shuffle_order_engine",       "knuth_b"                                                             },
    {"philox_engine",              "philox4x32 philox4x64"                                               },
    {"coroutine_handle",           "noop_coroutine_handle"                                               },
    {"counting_semaphore",         "binary_semaphore"                                                    },
};

}  // namespace

auto ReadableTypes::standard_alias(const clang::ClassTemplateSpecializationDecl& spec)
    -> const clang::TypedefNameDecl* {
    auto type = context.getCanonicalTagType(&spec);
    auto [cached, inserted] = aliases.try_emplace(type.getTypePtr(), nullptr);
    if(!inserted) {
        return cached->second;
    }
    const auto* typedefs = llvm::find_if(standard_typedefs, [&](const StandardTypedefs& entry) {
        return entry.pattern == spec.getSpecializedTemplate()->getName();
    });
    const clang::NamespaceDecl* outermost = nullptr;
    for(const auto* scope = spec.getDeclContext(); scope; scope = scope->getParent()) {
        if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(scope)) {
            outermost = ns;
        }
    }
    if(typedefs == std::end(standard_typedefs) || !outermost || !outermost->isStdNamespace()) {
        return nullptr;
    }

    // Each name is looked up on its own: a unit built on a PCH reads only
    // the declarations found, where walking namespace std would read it all.
    const clang::TypedefNameDecl* found = nullptr;
    for(auto name: llvm::split(typedefs->names, ' ')) {
        const clang::DeclContext* scope = outermost;
        if(auto [outer, last] = name.rsplit("::"); !last.empty()) {
            scope = scope->lookup(&context.Idents.get(outer)).find_first<clang::NamespaceDecl>();
            if(!scope) {
                continue;
            }
            name = last;
        }
        // An alias written as another one (MSVC's `wstreampos = streampos`)
        // does not name the type a second time.
        const auto* alias =
            scope->lookup(&context.Idents.get(name)).find_first<clang::TypedefNameDecl>();
        if(!alias || !llvm::isa<clang::TemplateSpecializationType>(alias->getUnderlyingType()) ||
           !context.hasSameType(alias->getUnderlyingType(), type)) {
            continue;
        }
        if(found) {
            return nullptr;
        }
        found = alias;
    }
    cached->second = found;
    return found;
}

auto ReadableTypes::standard_name(const clang::PredefinedSugarType& type)
    -> const clang::TypedefNameDecl* {
    auto [cached, inserted] = aliases.try_emplace(&type, nullptr);
    if(!inserted) {
        return cached->second;
    }
    llvm::StringRef name;
    switch(type.getKind()) {
        case clang::PredefinedSugarType::Kind::SizeT: name = "size_t"; break;
        case clang::PredefinedSugarType::Kind::PtrdiffT: name = "ptrdiff_t"; break;
        // POSIX's ssize_t is no standard name.
        case clang::PredefinedSugarType::Kind::SignedSizeT: return nullptr;
    }
    auto find = [&](const clang::DeclContext* scope) -> const clang::TypedefNameDecl* {
        const auto* alias =
            scope->lookup(&context.Idents.get(name)).find_first<clang::TypedefNameDecl>();
        return alias && context.hasSameType(alias->getUnderlyingType(), type.desugar()) ? alias
                                                                                        : nullptr;
    };
    const auto* global = context.getTranslationUnitDecl();
    const auto* std_scope =
        global->lookup(&context.Idents.get("std")).find_first<clang::NamespaceDecl>();
    const auto* found = std_scope ? find(std_scope) : nullptr;
    cached->second = found ? found : find(global);
    return cached->second;
}

auto ReadableTypes::readable(clang::QualType type) -> clang::QualType {
    auto qualified = [&](clang::QualType inner) {
        return context.getQualifiedType(inner, type.getLocalQualifiers());
    };
    const auto* T = type.getTypePtr();
    /// The compiler's own names for the type of `sizeof` and of a pointer
    /// difference (`__size_t`), which no code spells.
    if(const auto* predefined = llvm::dyn_cast<clang::PredefinedSugarType>(T)) {
        if(const auto* alias = standard_name(*predefined)) {
            auto scope = scoped ? written_scope(alias) : std::nullopt;
            return qualified(
                context.getTypedefType(clang::ElaboratedTypeKeyword::None, scope, alias));
        }
        return qualified(predefined->desugar());
    }
    if(const auto* deduced = llvm::dyn_cast<clang::AutoType>(T);
       deduced && deduced->getDeducedKind() == clang::DeducedKind::Deduced) {
        return qualified(readable(deduced->getDeducedType()));
    }
    if(const auto* subst = llvm::dyn_cast<clang::SubstTemplateTypeParmType>(T)) {
        return qualified(readable(subst->getReplacementType()));
    }
    if(const auto* alias = llvm::dyn_cast<clang::TypedefType>(T);
       alias && internal(alias->getDecl())) {
        return qualified(readable(alias->desugar()));
    }
    if(const auto* pointer = llvm::dyn_cast<clang::PointerType>(T)) {
        return qualified(context.getPointerType(readable(pointer->getPointeeType())));
    }
    if(const auto* reference = llvm::dyn_cast<clang::LValueReferenceType>(T)) {
        return qualified(context.getLValueReferenceType(readable(reference->getPointeeType())));
    }
    if(const auto* reference = llvm::dyn_cast<clang::RValueReferenceType>(T)) {
        return qualified(context.getRValueReferenceType(readable(reference->getPointeeType())));
    }

    clang::TemplateName name;
    llvm::ArrayRef<clang::TemplateArgument> arguments;
    if(const auto* TST = llvm::dyn_cast<clang::TemplateSpecializationType>(T)) {
        if(TST->isTypeAlias() && internal(TST->getTemplateName().getAsTemplateDecl())) {
            return qualified(readable(TST->getAliasedType()));
        }
        name = TST->getTemplateName();
        arguments = TST->template_arguments();
    } else if(const auto* record = llvm::dyn_cast<clang::RecordType>(T)) {
        const auto* spec =
            llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record->getDecl());
        if(!spec) {
            return type;
        }
        if(const auto* alias = standard_alias(*spec)) {
            auto scope = scoped ? written_scope(alias) : std::nullopt;
            return qualified(
                context.getTypedefType(clang::ElaboratedTypeKeyword::None, scope, alias));
        }
        name = clang::TemplateName(spec->getSpecializedTemplate());
        // A member template keeps the class around it (`Outer::Inner`).
        if(const auto* outer = llvm::dyn_cast<clang::CXXRecordDecl>(spec->getDeclContext())) {
            name = context.getQualifiedTemplateName(
                clang::NestedNameSpecifier(context.getCanonicalTagType(outer).getTypePtr()),
                /*TemplateKeyword=*/false,
                name);
        }
        arguments = spec->getTemplateArgs().asArray();
    } else {
        return type;
    }

    llvm::SmallVector<clang::TemplateArgument, 4> rewritten;
    bool changed = false;
    for(const auto& argument: arguments) {
        rewritten.push_back(readable(argument));
        changed |= !rewritten.back().structurallyEquals(argument);
    }
    if(!changed) {
        return type;
    }
    llvm::SmallVector<clang::TemplateArgument, 4> canonical;
    for(const auto& argument: arguments) {
        canonical.push_back(context.getCanonicalTemplateArgument(argument));
    }
    return qualified(context.getTemplateSpecializationType(clang::ElaboratedTypeKeyword::None,
                                                           name,
                                                           rewritten,
                                                           canonical,
                                                           context.getCanonicalType(type)));
}

auto ReadableTypes::readable(const clang::TemplateArgument& argument) -> clang::TemplateArgument {
    if(argument.getKind() == clang::TemplateArgument::Type) {
        return clang::TemplateArgument(readable(argument.getAsType()),
                                       /*isNullPtr=*/false,
                                       argument.getIsDefaulted());
    }
    if(argument.getKind() == clang::TemplateArgument::Pack) {
        llvm::SmallVector<clang::TemplateArgument, 4> elements;
        for(const auto& element: argument.pack_elements()) {
            elements.push_back(readable(element));
        }
        return clang::TemplateArgument::CreatePackCopy(context, elements);
    }
    return argument;
}

}  // namespace clice::display
