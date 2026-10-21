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
import :support.text;
import :syntax.lexer;
import :syntax.lexical_scan;

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
        /// Implicit class template specializations carry no written arguments;
        /// fall back to the converted ones.
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
            auto readable = options.readable->readable(context.getCanonicalTagType(spec),
                                                       decl.getDeclContext(),
                                                       options.source);
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
    // A lambda's type names no file: `(lambda)`.
    policy.AnonymousTagNameStyle =
        std::to_underlying(clang::PrintingPolicy::AnonymousTagMode::Plain);
    std::string text;
    llvm::raw_string_ostream os(text);
    clang::printTemplateArgumentList(os, args, policy, params);
    return text;
}

/// A simple wrapper for `clang::desugarForDiagnostic` that provides optional
/// semantic.
static std::optional<clang::QualType> desugar(clang::ASTContext& context, clang::QualType type) {
    /// A dependent alias template's specialization ReadableTypes read anew
    /// stands for itself: nothing tells what it aliases.
    if(const auto* alias = llvm::dyn_cast<clang::TemplateSpecializationType>(type.getTypePtr());
       alias && alias->isTypeAlias()) {
        const auto* aliased = alias->getAliasedType()->getAs<clang::TemplateSpecializationType>();
        const auto* target = aliased ? aliased->getTemplateName().getAsTemplateDecl() : nullptr;
        if(target && target->getCanonicalDecl() ==
                         alias->getTemplateName().getAsTemplateDecl()->getCanonicalDecl()) {
            return std::nullopt;
        }
    }
    bool should_aka = false;
    auto desugared = clang::desugarForDiagnostic(context, type, should_aka);
    if(!should_aka) {
        return std::nullopt;
    }

    return desugared;
}

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
/// form historically used. It diverges from DeclarationName::print where
/// a deduction guide prints the plain template name.
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
            result += R"(operator"")";
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
            /// FIXME: Objective-C message completion names methods by their selector:
            /// this aborts Debug builds and is undefined in Release; `name.print` would
            /// spell them.
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

auto symbol_name(const clang::NamedDecl* decl) -> SymbolName {
    // A deduction guide is listed under its label rather than the
    // template's name, which completion prefers for its candidates.
    if(llvm::isa<clang::CXXDeductionGuideDecl>(decl)) {
        return {.text = name_of(decl)};
    }
    auto name = name_of(decl, {.suppress_ctor_template_args = true, .qualified = false});
    if(name.empty()) {
        return {.text = name_of(decl), .unnamed = true};
    }
    return {.text = std::move(name)};
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
    policy.PrintInjectedClassNameWithArguments = !options.injected_as_written;
    policy.ConstantsAsWritten = options.constants_as_written;
    policy.SuppressTemplateArgsInCXXConstructors = options.suppress_ctor_template_args;
    policy.CleanUglifiedParameters = options.clean_uglified;
    policy.AnonymousTagNameStyle =
        std::to_underlying(clang::PrintingPolicy::AnonymousTagMode::Plain);
    // As clang's diagnostics do: C's `_Bool` reads `bool` where a macro
    // (<stdbool.h>) spells it so.
    if(!policy.Bool) {
        // A macro a precompiled header defines loads only when something looks
        // its name up: a file that never spells `bool` leaves the name out of
        // date, its macro flag unset.
        auto* name = context.getBoolName();
        if(name->isOutOfDate()) {
            context.getExternalSource()->updateOutOfDateIdentifier(*name);
        }
        policy.Bool = name->hasMacroDefinition();
    }
    // FIXME: SuppressStrongLifetime stays off, so an Objective-C ARC parameter
    // written `id __strong value` shows `__strong` in signature help and
    // completion labels.
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
    std::optional<EnclosingScopes> scopes;
    if(options.scope) {
        policy.Callbacks = &scopes.emplace(options.scope);
    }
    if(options.readable && !type.isNull()) {
        type =
            options.readable->readable(type, options.scope, options.source, options.specialization);
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
        /// FIXME: the aka has no size cap: aliases whose arguments repeat aliases
        /// (`using L1 = Foo<L0, L0>;`, n deep) spell k^n leaves, megabytes of hover
        /// text at six levels of ten arguments.
        if(auto desugared = desugar(context, sugared)) {
            if(options.readable) {
                desugared =
                    options.readable->underlying(*desugared, options.scope, options.specialization);
            }
            if(auto aka = desugared->getAsString(policy); aka != result.text) {
                result.aka = std::move(aka);
            }
        }
    }
    return result;
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
    auto width = context.getTypeSize(element);
    bool after_hex = false;
    for(std::size_t index = 0; index < characters.size(); index += 1) {
        if(index >= limit) {
            text += "...";
            break;
        }
        auto c = characters[index];
        bool follows_hex = std::exchange(after_hex, false);
        switch(c) {
            case '"': text += R"(\")"; continue;
            case '\\': text += R"(\\)"; continue;
            case '\n': text += R"(\n)"; continue;
            case '\t': text += R"(\t)"; continue;
            default: break;
        }
        // Octal, three digits at most: a hex escape would run on into a hex
        // digit after it.
        auto escape = [&] {
            text += '\\';
            for(auto shift: {6, 3, 0}) {
                text += static_cast<char>('0' + ((c >> shift) & 7));
            }
        };
        if(c < 0x20 || c == 0x7f ||
           (follows_hex && c < 0x80 && llvm::isHexDigit(static_cast<char>(c)))) {
            escape();
            continue;
        }
        if(width == 8) {
            /// A UTF-8 sequence passes through as the bytes it is; any other
            /// byte past ASCII is escaped.
            auto length = llvm::getNumBytesForUTF8(static_cast<llvm::UTF8>(c));
            std::array<llvm::UTF8, 4> sequence{};
            if(length > sequence.size() || index + length > characters.size()) {
                escape();
                continue;
            }
            for(unsigned offset = 0; offset < length; offset += 1) {
                sequence[offset] = static_cast<llvm::UTF8>(characters[index + offset]);
            }
            if(!llvm::isLegalUTF8Sequence(sequence.data(), sequence.data() + length)) {
                escape();
                continue;
            }
            text.append(reinterpret_cast<const char*>(sequence.data()), length);
            index += length - 1;
            continue;
        }
        if(width == 16 && c >= 0xD800 && c < 0xDC00 && index + 1 < characters.size() &&
           characters[index + 1] >= 0xDC00 && characters[index + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (characters[index + 1] - 0xDC00);
            index += 1;
        }
        std::array<char, 4> buffer;
        char* end = buffer.data();
        if(llvm::ConvertCodePointToUTF8(c, end)) {
            text.append(buffer.data(), end);
        } else {
            /// No universal character name spells what is no character, a
            /// lone surrogate.
            text += R"(\x)";
            text += llvm::utohexstr(c, /*LowerCase=*/true);
            after_hex = true;
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
        /// `signed char` and `unsigned char` (`std::uint8_t`) hold bytes.
        const auto* array = context.getAsArrayType(type);
        if(!array || !array->getElementType()->isAnyCharacterType() ||
           array->getElementType()->isSpecificBuiltinType(clang::BuiltinType::SChar) ||
           array->getElementType()->isSpecificBuiltinType(clang::BuiltinType::UChar)) {
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
        /// The pointer may have another type, `const void*`.
        return quoted(
            context,
            context.getAsArrayType(literal->getType())->getElementType().getUnqualifiedType(),
            characters);
    }
    return std::nullopt;
}

/// The shortest text that reads back as the same value: `3.14` rather than
/// `3.1400000000000001`, the digits a double holds. APFloat writes too few
/// digits for a whole number in exponent form (`1.5E+3` for 1500), so one
/// without the exponent is taken if more digits give it.
auto float_text(const llvm::APFloat& value) -> std::string {
    llvm::SmallString<32> text;
    std::string shortest;
    if(value.isFinite()) {
        for(unsigned precision = 1; precision < 40; precision += 1) {
            text.clear();
            value.toString(text, precision);
            llvm::APFloat read(value.getSemantics());
            llvm::cantFail(read.convertFromString(text, llvm::APFloat::rmNearestTiesToEven));
            if(!read.bitwiseIsEqual(value)) {
                continue;
            }
            if(!text.contains('E')) {
                return text.str().str();
            }
            if(shortest.empty()) {
                shortest = text.str();
            }
        }
    }
    if(!shortest.empty()) {
        return shortest;
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
        auto imaginary = float_text(constant.Val.getComplexFloatImag());
        return float_text(constant.Val.getComplexFloatReal()) +
               (llvm::StringRef(imaginary).starts_with("-") ? "" : "+") + imaginary + "i";
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
    /// FIXME: The keyword is always `class`: a parameter declared `typename`
    /// (wasDeclaredWithTypename) or a C++26 concept or variable template
    /// template parameter (templateParameterKind) prints wrong.
    os << "> class";
    return result;
}

/// The bodies of the lambdas in `stmt`, which its text leaves out.
void lambda_bodies(const clang::Stmt* stmt, llvm::SmallVectorImpl<clang::SourceRange>& bodies) {
    if(!stmt) {
        return;
    }
    if(const auto* lambda = llvm::dyn_cast<clang::LambdaExpr>(stmt)) {
        bodies.push_back(lambda->getBody()->getSourceRange());
        return;
    }
    for(const auto* child: stmt->children()) {
        lambda_bodies(child, bodies);
    }
}

/// The text of `range` on one line: its tokens as spelled, a space where
/// the source has space, a comment or a line break between two, the bodies
/// in `elided` as `{ ... }`; nullopt when a directive is in it, which no
/// line of code can hold.
auto token_text(const clang::ASTContext& context,
                clang::CharSourceRange range,
                llvm::ArrayRef<clang::SourceRange> elided) -> std::optional<std::string> {
    auto& sources = context.getSourceManager();
    auto [fid, begin] = sources.getDecomposedLoc(range.getBegin());
    auto end = sources.getFileOffset(range.getEnd());
    auto buffer = sources.getBufferData(fid);
    llvm::SmallVector<std::pair<std::uint32_t, std::uint32_t>> skipped;
    for(auto body: elided) {
        auto first = sources.getDecomposedLoc(body.getBegin());
        auto last = sources.getDecomposedLoc(body.getEnd());
        if(body.getBegin().isFileID() && body.getEnd().isFileID() && first.first == fid &&
           last.first == fid) {
            skipped.emplace_back(first.second, last.second);
        }
    }

    std::string text;
    std::uint32_t previous_end = begin;
    auto rest = buffer.substr(begin);
    Lexer lexer(rest, context.getLangOpts());
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        auto offset = begin + token.range.begin;
        if(offset >= end) {
            break;
        }
        if(token.is_directive_hash()) {
            return std::nullopt;
        }
        // A text opening with `module` or `export` lexes as a module
        // declaration's line, which an eod ends.
        if(token.is_eod()) {
            continue;
        }
        const auto* body = llvm::find_if(skipped, [&](const auto& skip) {
            return skip.first <= offset && offset <= skip.second;
        });
        if(body != skipped.end()) {
            if(offset == body->first) {
                text += text.empty() ? "{ ... }" : " { ... }";
            }
            previous_end = offset + token.range.length();
            continue;
        }
        if(!text.empty() && offset != previous_end) {
            text += ' ';
        }
        text += token.is_raw_string(rest) ? token.text(rest).str() : unspliced(token.text(rest));
        previous_end = offset + token.range.length();
    }
    return text;
}

/// `expr` as the source writes it, on one line; nullopt when the source
/// has a directive in it.
auto as_written(clang::ASTContext& context, const clang::Expr* expr, const Options& options)
    -> std::optional<std::string> {
    auto& sources = context.getSourceManager();
    auto range = clang::Lexer::makeFileCharRange(
        clang::CharSourceRange::getTokenRange(expr->getSourceRange()),
        sources,
        context.getLangOpts());
    if(range.isValid()) {
        // A constructor call that copy-initializes a parameter or a field
        // starts at its `=` (`std::optional<bool> sign = {}`).
        clang::Token first;
        if(!clang::Lexer::getRawToken(range.getBegin(), first, sources, context.getLangOpts()) &&
           first.is(clang::tok::equal)) {
            range.setBegin(first.getEndLoc());
        }
        llvm::SmallVector<clang::SourceRange> bodies;
        lambda_bodies(expr, bodies);
        return token_text(context, range, bodies);
    }

    // Written in a macro's body: the source holds the macro's text, not
    // this declaration's.
    auto policy = printing_policy(context, options);
    policy.ConstantsAsWritten = true;
    std::string text;
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

auto call_signature(const clang::FunctionDecl& function, const Options& options) -> std::string {
    llvm::SmallVector<std::string> parameters;
    bool object = function.hasCXXExplicitFunctionObjectParameter();
    for(unsigned index = object && !options.polish_for_declaration ? 1 : 0;
        index < function.getNumParams();
        index += 1) {
        auto written =
            parameter(*function.getParamDecl(index), parameter_name(function, index), options);
        parameters.push_back(object && index == 0 ? "this " + written : std::move(written));
    }
    if(function.isVariadic()) {
        parameters.push_back("...");
    }
    auto signature = "(" + llvm::join(parameters, ", ") + ")";
    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
       method && !function.hasCXXExplicitFunctionObjectParameter()) {
        signature += method_qualifiers(*method);
    }
    return signature;
}

auto param(const clang::ParmVarDecl* param, const Options& options) -> Param {
    assert(param);
    Param result;
    result.type = type(param->getASTContext(), param->getType(), options);
    auto named = decls::named_parameters(*param);
    if(!named.empty()) {
        result.name = named.front()->getName().str();
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
    auto text = type(param.getASTContext(), written, options, declarator).text;
    /// A parameter of function type, `int callback(int)`: clang puts the
    /// name in parentheses, `int (callback)(int)`.
    if(written->getAs<clang::FunctionProtoType>() && !name.empty()) {
        auto parenthesized = "(" + declarator + ")";
        if(auto at = text.find(parenthesized); at != std::string::npos) {
            text.replace(at, parenthesized.size(), declarator);
        }
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
        if(auto written = as_written(context, width, options)) {
            text += " : " + *written;
        }
    }
    if(const auto* init = field.getInClassInitializer()) {
        if(auto written = as_written(context, init, options)) {
            text += field.getInClassInitStyle() == clang::ICIS_ListInit ? " " : " = ";
            text += *written;
        }
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

/// A reserved name is the implementation's own (`_M_impl`, `__base`).
bool is_reserved(const clang::Decl& member) {
    const auto* named = llvm::dyn_cast<clang::NamedDecl>(&member);
    return named && clang::isReservedInAllContexts(named->isReserved(member.getLangOpts()));
}

/// Whether a class summary lists the member: its data and the types it
/// declares. A type declared twice is listed once, and a specialization of
/// a member template is left to the template.
bool is_listed(const clang::Decl& member) {
    if(member.isImplicit() || !member.isFirstDecl() || is_reserved(member) ||
       llvm::isa<clang::ClassTemplateSpecializationDecl, clang::VarTemplateSpecializationDecl>(
           member)) {
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
    } else if(const auto* var = llvm::dyn_cast<clang::VarDecl>(&member)) {
        type_start = var->getTypeSpecStartLoc();
        type = var->getType();
    } else if(const auto* typedef_decl = llvm::dyn_cast<clang::TypedefDecl>(&member)) {
        type_start = typedef_decl->getTypeSourceInfo()->getTypeLoc().getBeginLoc();
        type = typedef_decl->getUnderlyingType();
    } else {
        return false;
    }
    return type_start == tag.getBeginLoc() && types::unwrap(type)->getAs<clang::TagType>();
}

/// The members of `record` in the order its source writes them: a record
/// read from an AST file whose fields were loaded first, as its layout loads
/// them, lists them after the rest. Ordered by name, as the declarators of
/// `volatile struct { ... } a, b;` begin before the struct they declare.
llvm::SmallVector<const clang::Decl*> source_order(const clang::RecordDecl& record) {
    llvm::SmallVector<const clang::Decl*> decls;
    for(const auto* decl: record.decls()) {
        if(decl->getLocation().isValid()) {
            decls.push_back(decl);
        }
    }
    auto& sources = record.getASTContext().getSourceManager();
    llvm::stable_sort(decls, [&](const clang::Decl* left, const clang::Decl* right) {
        return sources.isBeforeInTranslationUnit(left->getLocation(), right->getLocation());
    });
    return decls;
}

/// Whether the alias names a tag declared with it and named by it alone, as
/// in `typedef struct { ... } Point;`.
bool declares_tag(const clang::TypedefNameDecl& alias) {
    const auto* tag = alias.getUnderlyingType()->getAsTagDecl();
    return tag && tag->getTypedefNameForAnonDecl() == &alias && tag->getDefinition();
}

/// Whether the declaration is a class template's instantiation's member, or
/// a function or variable template's instantiation: it holds what the
/// instantiation made of the template's, which no source writes.
bool instantiated(const clang::Decl& decl) {
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(&decl);
       var && (var->getInstantiatedFromStaticDataMember() ||
               llvm::isa<clang::VarTemplateSpecializationDecl>(var))) {
        return true;
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&decl);
       function && function->isTemplateInstantiation()) {
        return true;
    }
    for(const auto* context = decl.getDeclContext(); context; context = context->getParent()) {
        if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(context);
           record && record->getTemplateInstantiationPattern()) {
            return true;
        }
    }
    return false;
}

/// The unnamed tags a type names, in the order its text names them; one a
/// typedef names for linkage prints as the typedef.
void unnamed_tags(clang::QualType type, llvm::SmallVectorImpl<const clang::TagDecl*>& tags) {
    if(type.isNull()) {
        return;
    }
    const auto* node = type.getTypePtr();
    if(const auto* tag = llvm::dyn_cast<clang::TagType>(node)) {
        const auto* decl = tag->getDecl();
        const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl);
        if(!decl->getDeclName() && !decl->getTypedefNameForAnonDecl() &&
           !(record && record->isLambda())) {
            tags.push_back(decl);
        }
    } else if(const auto* pointer = llvm::dyn_cast<clang::PointerType>(node)) {
        unnamed_tags(pointer->getPointeeType(), tags);
    } else if(const auto* reference = llvm::dyn_cast<clang::ReferenceType>(node)) {
        unnamed_tags(reference->getPointeeTypeAsWritten(), tags);
    } else if(const auto* member = llvm::dyn_cast<clang::MemberPointerType>(node)) {
        unnamed_tags(member->getPointeeType(), tags);
    } else if(const auto* array = llvm::dyn_cast<clang::ArrayType>(node)) {
        unnamed_tags(array->getElementType(), tags);
    } else if(const auto* function = llvm::dyn_cast<clang::FunctionType>(node)) {
        unnamed_tags(function->getReturnType(), tags);
        if(const auto* prototype = llvm::dyn_cast<clang::FunctionProtoType>(function)) {
            for(auto param: prototype->param_types()) {
                unnamed_tags(param, tags);
            }
        }
    } else if(const auto* paren = llvm::dyn_cast<clang::ParenType>(node)) {
        unnamed_tags(paren->getInnerType(), tags);
    } else if(const auto* atomic = llvm::dyn_cast<clang::AtomicType>(node)) {
        unnamed_tags(atomic->getValueType(), tags);
    } else if(const auto* type_of = llvm::dyn_cast<clang::TypeOfType>(node)) {
        unnamed_tags(type_of->getUnmodifiedType(), tags);
    } else if(const auto* attributed = llvm::dyn_cast<clang::AttributedType>(node)) {
        unnamed_tags(attributed->getModifiedType(), tags);
    } else if(const auto* macro = llvm::dyn_cast<clang::MacroQualifiedType>(node)) {
        unnamed_tags(macro->getUnderlyingType(), tags);
    } else if(const auto* adjusted = llvm::dyn_cast<clang::AdjustedType>(node)) {
        unnamed_tags(adjusted->getOriginalType(), tags);
    } else if(const auto* expansion = llvm::dyn_cast<clang::PackExpansionType>(node)) {
        unnamed_tags(expansion->getPattern(), tags);
    } else if(const auto* id = llvm::dyn_cast<clang::TemplateSpecializationType>(node)) {
        for(const auto& argument: id->template_arguments()) {
            if(argument.getKind() == clang::TemplateArgument::Type) {
                unnamed_tags(argument.getAsType(), tags);
            }
        }
    }
}

/// The attribute as clang prints it, its expression arguments as code:
/// clang prints a list of them by their addresses,
/// `release_capability(0x55f4c48d3398)`.
auto pretty(const clang::Attr& attr, const clang::PrintingPolicy& policy) -> std::string {
    std::string text;
    llvm::raw_string_ostream os(text);
    attr.printPretty(os, policy);

    struct Arguments : clang::RecursiveASTVisitor<Arguments> {
        bool TraverseStmt(clang::Stmt* stmt) {
            if(auto* expr = llvm::dyn_cast_or_null<clang::Expr>(stmt)) {
                exprs.push_back(expr);
            }
            return true;
        }

        llvm::SmallVector<const clang::Expr*> exprs;
    } arguments;

    arguments.TraverseAttr(const_cast<clang::Attr*>(&attr));
    for(const auto* expr: arguments.exprs) {
        std::string address;
        llvm::raw_string_ostream(address) << static_cast<const void*>(expr);
        if(auto at = text.find(address); at != std::string::npos) {
            std::string code;
            llvm::raw_string_ostream stream(code);
            expr->printPretty(stream, nullptr, policy);
            text.replace(at, address.size(), code);
        }
    }
    return text;
}

/// The attribute specifiers a declaration writes, where it writes them.
struct WrittenAttributes {
    /// Before the declaration.
    std::vector<std::string> before;

    /// After its name.
    std::vector<std::string> after;
};

auto join(llvm::ArrayRef<std::string> texts) -> std::string {
    return llvm::join(texts, " ");
}

/// Prints declarations as a card shows them: a written one as its source
/// writes it, an instantiated one as what the instantiation made of the
/// template's — its types as a reader knows them, its values evaluated —
/// and a class or an enum with its members summarized.
struct DeclarationPrinter {
    clang::ASTContext& context;

    /// Prints what the printer leaves to clang: template heads, expressions.
    const clang::PrintingPolicy& policy;

    const Options& options;
    TokenCount token_count;

    /// How many more members to print, those of nested tags included.
    std::uint32_t budget;

    /// The macros printed attributes are written with.
    std::vector<AttributeMacro>& macros;

    /// The declaration whose attributes written before it go apart, on a
    /// line of their own.
    const clang::Decl* apart = nullptr;

    /// Whether one more member fits; marks the cut when it does not.
    bool fits(std::string& text) {
        if(budget == 0) {
            text += "\n// ...";
            return false;
        }
        budget -= 1;
        return true;
    }

    auto printed(const clang::Attr& attr) -> std::string {
        auto text = pretty(attr, policy);
        /// clang prints an absent message as an empty one:
        /// `[[nodiscard("")]]`.
        llvm::StringRef trimmed = llvm::StringRef(text).trim();
        if(auto empty = trimmed.find(R"((""))");
           empty != llvm::StringRef::npos &&
           (trimmed.substr(empty + 4) == "]]" || trimmed.substr(empty + 4) == "))")) {
            return (trimmed.take_front(empty) + trimmed.drop_front(empty + 4)).str();
        }
        return trimmed.str();
    }

    /// Whether the macros an attribute at `location` comes out of write
    /// nothing but attributes: one that also writes a specifier, as
    /// `#define FORCE_INLINE static inline __attribute__((always_inline))`
    /// does, names no attribute.
    bool attributes_only(clang::SourceLocation location) {
        auto& sources = context.getSourceManager();
        for(auto at = location; at.isMacroID(); at = sources.getImmediateMacroCallerLoc(at)) {
            if(!sources.isMacroBodyExpansion(at)) {
                continue;
            }
            auto spelled = sources.getImmediateSpellingLoc(at);
            if(!spelled.isFileID() || sources.isWrittenInScratchSpace(spelled)) {
                continue;
            }
            /// The `#define`, from the line it begins on.
            auto [file, offset] = sources.getDecomposedLoc(spelled);
            auto content = sources.getBufferData(file).substr(
                logical_line_begin(sources.getBufferData(file), offset));
            Lexer lexer(content, context.getLangOpts());
            llvm::SmallVector<Token, 16> words;
            lexer.advance();
            for(auto token = lexer.advance(); !token.is_eod() && !token.is_eof();
                token = lexer.advance()) {
                words.push_back(token);
            }
            auto definition = macro_definition(words);
            if(!definition) {
                continue;
            }
            int depth = 0;
            for(const auto& token: definition->body) {
                if(token.kind == clang::tok::l_paren || token.kind == clang::tok::l_square) {
                    depth += 1;
                } else if(token.kind == clang::tok::r_paren || token.kind == clang::tok::r_square) {
                    depth -= 1;
                } else if(depth == 0 && token.is_identifier()) {
                    auto name = token.text(content);
                    if(context.Idents.get(name).isKeyword(context.getLangOpts()) &&
                       !llvm::is_contained(
                           {"__attribute__", "__attribute", "__declspec", "alignas", "_Alignas"},
                           name)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    /// Every attribute the declaration writes, in the order it writes them:
    /// one a macro writes by the macro's name, the macro noted with what it
    /// expands to.
    auto written(const clang::Decl& decl) -> WrittenAttributes {
        auto& sources = context.getSourceManager();
        auto name = sources.getExpansionLoc(decl.getLocation());
        bool instance = instantiated(decl);
        /// The macro uses already printed, by where they begin.
        llvm::SmallVector<std::pair<clang::SourceLocation, std::size_t>> uses;
        using Placed = llvm::SmallVector<std::pair<clang::SourceLocation, std::string>>;
        Placed before;
        Placed after;
        for(const auto* attr: decl.attrs()) {
            if(attr->isImplicit() || attr->isInherited() ||
               attr->getSyntax() == clang::AttributeCommonInfo::AS_Pragma ||
               (llvm::isa<clang::FinalAttr>(attr) && llvm::isa<clang::TagDecl>(decl))) {
                continue;
            }
            std::string text;
            auto location = attr->getRange().getBegin();
            if(location.isInvalid()) {
                location = name;
            }
            if(decls::applied_by_pragma(context, location)) {
                continue;
            }
            if(location.isMacroID()) {
                auto expansion = sources.getExpansionRange(location);
                // A macro that writes the declaration itself names no
                // attribute.
                bool whole = !sources.isBeforeInTranslationUnit(name, expansion.getBegin()) &&
                             !sources.isBeforeInTranslationUnit(expansion.getEnd(), name);
                bool named = !whole && attributes_only(location);
                location = expansion.getBegin();
                if(named) {
                    auto use = llvm::find_if(uses, [&](const auto& entry) {
                        return entry.first == location;
                    });
                    if(use != uses.end()) {
                        macros[use->second].attributes += " " + printed(*attr);
                        continue;
                    }
                    text = clang::Lexer::getSourceText(expansion, sources, context.getLangOpts())
                               .str();
                    auto known = llvm::find_if(macros, [&](const AttributeMacro& macro) {
                        return macro.use == text;
                    });
                    if(known == macros.end()) {
                        uses.emplace_back(location, macros.size());
                        macros.push_back({text, printed(*attr)});
                    }
                }
            }
            if(text.empty()) {
                text = instance ? printed(*attr) : attribute(*attr, context);
            }
            auto& side =
                location == name ||
                        sources.isBeforeInTranslationUnit(sources.getExpansionLoc(location), name)
                    ? before
                    : after;
            /// By where each ends: an attribute with arguments in
            /// `[[using gnu: used, aligned(8)]]` starts at the scope.
            if(llvm::none_of(side, [&](const auto& entry) { return entry.second == text; })) {
                side.emplace_back(attr->getRange().isValid() ? attr->getRange().getEnd() : location,
                                  std::move(text));
            }
        }
        auto ordered = [&](Placed& side) {
            llvm::stable_sort(side, [&](const auto& left, const auto& right) {
                return sources.isBeforeInTranslationUnit(sources.getExpansionLoc(left.first),
                                                         sources.getExpansionLoc(right.first));
            });
            std::vector<std::string> texts;
            for(auto& entry: side) {
                texts.push_back(std::move(entry.second));
            }
            return texts;
        };
        return {.before = ordered(before), .after = ordered(after)};
    }

    /// The attributes written before `decl`, ahead of its text: on their
    /// own line for the card's declaration, which definition() puts there.
    auto leading(const clang::Decl& decl, const WrittenAttributes& attributes) -> std::string {
        if(&decl == apart || attributes.before.empty()) {
            return {};
        }
        return join(attributes.before) + " ";
    }

    /// The options a type of `decl` prints with, shown in `scope`: a written
    /// declaration's types as written, an instantiated one's as a reader
    /// knows them.
    auto type_options(const clang::Decl& decl, const clang::DeclContext* scope) -> Options {
        auto result = options;
        result.show_aka = false;
        result.scope = scope;
        /// A template's member shown through a dependent specialization of
        /// it reads the template's parameters as its arguments.
        if(!instantiated(decl)) {
            result.resolve_decltype = false;
            if(!options.specialization) {
                result.readable = nullptr;
                result.injected_as_written = true;
            }
        }
        return result;
    }

    /// A type with its declarator, an unnamed tag it names printed with its
    /// members: `struct { int x; } *point`.
    auto declarator_type(clang::QualType type, llvm::StringRef declarator, const Options& shown)
        -> std::string {
        auto text = display::type(context, type, shown, declarator).text;
        llvm::SmallVector<const clang::TagDecl*> tags;
        if(shown.readable) {
            type = shown.readable->readable(type, shown.scope, shown.source, shown.specialization);
        }
        unnamed_tags(type, tags);
        if(tags.empty()) {
            return text;
        }
        const static std::regex placeholder(
            R"((?:(?:struct|union|class|enum) )?\((?:unnamed|anonymous)(?: (?:struct|union|class|enum))?\))");
        std::string result;
        std::size_t copied = 0;
        std::size_t index = 0;
        for(std::sregex_iterator match(text.begin(), text.end(), placeholder), end;
            match != end && index < tags.size();
            ++match, index += 1) {
            auto at = static_cast<std::size_t>(match->position());
            result.append(text, copied, at - copied);
            result += tags[index]->getKindName();
            result += body(*tags[index]);
            copied = at + static_cast<std::size_t>(match->length());
        }
        result.append(text, copied);
        return result;
    }

    /// A declarator after the first of a declaration: `*b` in
    /// `struct { ... } a, *b;`.
    auto later_declarator(clang::QualType type, llvm::StringRef declarator, const Options& shown)
        -> std::string {
        if(shown.readable) {
            type = shown.readable->readable(type, shown.scope, shown.source, shown.specialization);
        }
        auto later = printing_policy(context, shown);
        later.SuppressSpecifiers = true;
        std::string text;
        llvm::raw_string_ostream os(text);
        type.print(os, later, declarator);
        return text;
    }

    /// An initializer as a card shows it: as written, or for an
    /// instantiation, its value or what the instantiation made of it;
    /// nullopt when it hides it.
    auto initializer(const clang::Expr* init, bool instance) -> std::optional<std::string> {
        if(hides_initializer(init, policy, token_count)) {
            return std::nullopt;
        }
        if(!instance) {
            return as_written(context, init, options);
        }
        /// An enumerator's value prints as a number, which no enumeration
        /// takes; the expression names it.
        if(!init->isValueDependent() && !init->getType()->isEnumeralType()) {
            if(clang::Expr::EvalResult value; init->EvaluateAsRValue(value, context) &&
                                              !value.Val.isStruct() && !value.Val.isUnion() &&
                                              !value.Val.isArray() && !value.Val.isLValue()) {
                return value.Val.getAsString(context, init->getType());
            }
        }
        auto instance_policy = policy;
        instance_policy.ConstantsAsWritten = false;
        std::string text;
        llvm::raw_string_ostream os(text);
        init->printPretty(os, nullptr, instance_policy, 0, "\n", &context);
        return text;
    }

    /// The initializer of a class template's static data member or member
    /// variable template, for an instantiation that has none of its own
    /// yet: clang instantiates it only where it is used. One that depends
    /// on the template's arguments says nothing until then.
    static auto template_initializer(const clang::VarDecl& var) -> const clang::Expr* {
        const clang::VarDecl* pattern = var.getInstantiatedFromStaticDataMember();
        if(const auto* described = var.getDescribedVarTemplate()) {
            if(const auto* from = described->getInstantiatedFromMemberTemplate()) {
                pattern = from->getTemplatedDecl();
            }
        }
        if(!pattern || var.getInit()) {
            return nullptr;
        }
        const auto* init = pattern->getInit();
        return init && !init->isValueDependent() ? init : nullptr;
    }

    /// The argument an instantiation's static data member is initialized
    /// with when its initializer, not instantiated yet, only names a
    /// parameter of the class template: `integral_constant`'s `value = __v`.
    auto argument_initializer(const clang::VarDecl& var) -> std::optional<std::string> {
        const auto* pattern = var.getInstantiatedFromStaticDataMember();
        const auto* spec =
            llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(var.getDeclContext());
        if(!pattern || !pattern->getInit() || !spec) {
            return std::nullopt;
        }
        const auto* ref =
            llvm::dyn_cast<clang::DeclRefExpr>(pattern->getInit()->IgnoreParenImpCasts());
        const auto* param =
            ref ? llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(ref->getDecl()) : nullptr;
        if(!param) {
            return std::nullopt;
        }
        /// A parameter of the template the class is instantiated from, not
        /// of a template around it.
        const auto* record = spec->getTemplateInstantiationPattern();
        const clang::TemplateParameterList* params = nullptr;
        if(const auto* partial =
               llvm::dyn_cast_if_present<clang::ClassTemplatePartialSpecializationDecl>(record)) {
            params = partial->getTemplateParameters();
        } else if(const auto* described = record ? record->getDescribedClassTemplate() : nullptr) {
            params = described->getTemplateParameters();
        }
        const auto& arguments = spec->getTemplateInstantiationArgs();
        if(!params || param->getIndex() >= params->size() ||
           params->getParam(param->getIndex()) != param || param->getIndex() >= arguments.size() ||
           arguments[param->getIndex()].getKind() != clang::TemplateArgument::Integral) {
            return std::nullopt;
        }
        std::string text;
        llvm::raw_string_ostream os(text);
        arguments[param->getIndex()].print(policy, os, /*IncludeType=*/false);
        return text;
    }

    /// The text of a variable's initializer after its declarator, its `=`,
    /// parentheses or braces included; empty for none.
    auto variable_initializer(const clang::VarDecl& var, bool instance) -> std::string {
        const auto* init = var.getInit();
        if(!init) {
            if(const auto* pattern = template_initializer(var)) {
                if(auto text = initializer(pattern, false)) {
                    return " = " + *text;
                }
            }
            // FIXME: an initializer computed from the arguments (`= Size * 2`) or naming a
            // parameter of an enclosing template shows nothing until something uses the
            // member: clang instantiates it only there, and display has no Sema to do it.
            if(auto argument = argument_initializer(var)) {
                return " = " + *argument;
            }
            return {};
        }
        /// A range-for's variable is initialized from the range, which the
        /// loop writes.
        if(var.isCXXForRangeDecl()) {
            return {};
        }
        if(const auto* construct = llvm::dyn_cast<clang::CXXConstructExpr>(init->IgnoreImplicit());
           construct && var.getInitStyle() == clang::VarDecl::CallInit &&
           !construct->isListInitialization() &&
           (construct->getNumArgs() == 0 || construct->getArg(0)->isDefaultArgument())) {
            return {};
        }
        if(var.getInitStyle() == clang::VarDecl::CInit) {
            auto text = initializer(init, instance);
            return text ? " = " + *text : std::string();
        }
        if(hides_initializer(init, policy, token_count)) {
            return {};
        }
        /// Parentheses or braces after the declarator, which the
        /// initializer's own range may leave out (`Point p(1, 2)`).
        auto& sources = context.getSourceManager();
        auto declarator_end = var.getLocation();
        if(const auto* written = var.getTypeSourceInfo();
           written &&
           sources.isBeforeInTranslationUnit(declarator_end, written->getTypeLoc().getEndLoc())) {
            declarator_end = written->getTypeLoc().getEndLoc();
        }
        /// Past the attributes after the declarator, which the declarator
        /// shows already, and the brackets closing them.
        bool attributed = false;
        for(const auto* attr: var.attrs()) {
            auto end = sources.getExpansionRange(attr->getRange()).getEnd();
            if(!attr->isImplicit() && sources.isBeforeInTranslationUnit(declarator_end, end) &&
               sources.isBeforeInTranslationUnit(end,
                                                 sources.getExpansionLoc(init->getBeginLoc()))) {
                declarator_end = end;
                attributed = true;
            }
        }
        /// A scalar's parentheses are no part of its initializer: `int x(5)`.
        auto init_end = sources.getExpansionLoc(init->getEndLoc());
        if(var.getInitStyle() == clang::VarDecl::CallInit) {
            if(auto next = clang::Lexer::findNextToken(init_end, sources, context.getLangOpts());
               next && next->is(clang::tok::r_paren)) {
                init_end = next->getLocation();
            }
        }
        auto range = clang::Lexer::makeFileCharRange(
            clang::CharSourceRange::getTokenRange(declarator_end, init_end),
            sources,
            context.getLangOpts());
        if(!instance && range.isValid()) {
            auto start = clang::Lexer::getLocForEndOfToken(range.getBegin(),
                                                           0,
                                                           sources,
                                                           context.getLangOpts());
            llvm::SmallVector<clang::SourceRange> bodies;
            lambda_bodies(init, bodies);
            if(auto text = token_text(context,
                                      clang::CharSourceRange::getCharRange(start, range.getEnd()),
                                      bodies)) {
                return attributed ? llvm::StringRef(*text).ltrim(" ])").str() : *text;
            }
            return {};
        }
        auto text = initializer(init, instance);
        if(!text) {
            return {};
        }
        bool parens = var.getInitStyle() != clang::VarDecl::ListInit &&
                      !llvm::isa<clang::ParenListExpr>(init);
        return parens ? "(" + *text + ")" : *text;
    }

    auto variable(const clang::VarDecl& var) -> std::string {
        auto attributes = written(var);
        bool instance = instantiated(var);
        auto shown = type_options(var, var.getDeclContext());
        auto text = leading(var, attributes);
        if(var.isInlineSpecified()) {
            text += "inline ";
        }
        if(var.getStorageClass() != clang::SC_None) {
            text += clang::VarDecl::getStorageClassSpecifierString(var.getStorageClass());
            text += ' ';
        }
        switch(var.getTSCSpec()) {
            case clang::TSCS_unspecified: break;
            case clang::TSCS___thread: text += "__thread "; break;
            case clang::TSCS__Thread_local: text += "_Thread_local "; break;
            case clang::TSCS_thread_local: text += "thread_local "; break;
        }
        auto type = instance || !var.getTypeSourceInfo() ? var.getType()
                                                         : var.getTypeSourceInfo()->getType();
        if(var.isConstexpr()) {
            text += "constexpr ";
            type.removeLocalConst();
        }
        auto init = variable_initializer(var, instance);
        /// An array whose initializer is left out shows the bound it gives.
        if(const auto* bound = context.getAsConstantArrayType(var.getType());
           init.empty() && bound && type->isIncompleteArrayType()) {
            type = context.getQualifiedType(
                context.getConstantArrayType(context.getAsArrayType(type)->getElementType(),
                                             bound->getSize(),
                                             nullptr,
                                             clang::ArraySizeModifier::Normal,
                                             0),
                type.getLocalQualifiers());
        }
        std::string declarator = var.getName().str();
        if(!attributes.after.empty()) {
            declarator += " " + join(attributes.after);
        }
        text += declarator_type(type, declarator, shown);
        return text + init;
    }

    auto field(const clang::FieldDecl& field) -> std::string {
        auto attributes = written(field);
        bool instance = instantiated(field);
        auto shown = type_options(field, field.getDeclContext());
        auto text = leading(field, attributes);
        if(field.isMutable()) {
            text += "mutable ";
        }
        auto type = instance || !field.getTypeSourceInfo() ? field.getType()
                                                           : field.getTypeSourceInfo()->getType();
        std::string declarator = field.getName().str();
        if(!attributes.after.empty()) {
            declarator += (declarator.empty() ? "" : " ") + join(attributes.after);
        }
        text += declarator_type(type, declarator, shown);
        if(const auto* width = field.getBitWidth()) {
            if(auto value = initializer(width, instance)) {
                text += " : " + *value;
            }
        }
        /// Clang instantiates a default member initializer where it is used.
        const auto* init = field.getInClassInitializer();
        bool pattern = false;
        if(const auto* from = instance && !init ? pattern_field(field) : nullptr;
           from && from->getInClassInitializer() &&
           !from->getInClassInitializer()->isValueDependent()) {
            init = from->getInClassInitializer();
            pattern = true;
        }
        if(init) {
            if(auto value = initializer(init, instance && !pattern)) {
                text += field.getInClassInitStyle() == clang::ICIS_ListInit ? " " : " = ";
                text += *value;
            }
        }
        return text;
    }

    /// The field of the template an instantiation's field is made from.
    static auto pattern_field(const clang::FieldDecl& field) -> const clang::FieldDecl* {
        const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(field.getParent());
        const auto* pattern = record ? record->getTemplateInstantiationPattern() : nullptr;
        if(!pattern) {
            return nullptr;
        }
        auto fields = pattern->fields();
        auto it = std::next(fields.begin(), field.getFieldIndex());
        return it != fields.end() ? *it : nullptr;
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

    auto alias(const clang::TypedefNameDecl& alias) -> std::string {
        auto attributes = written(alias);
        auto text = leading(alias, attributes);
        bool is_using = llvm::isa<clang::TypeAliasDecl>(alias);
        auto name = alias.getName().str();
        if(!attributes.after.empty()) {
            name += " " + join(attributes.after);
        }
        /// Clang prints the tag under the alias's name, `typedef struct Point
        /// Point`, a struct C does not know.
        if(declares_tag(alias)) {
            const auto* tag = alias.getUnderlyingType()->getAsTagDecl()->getDefinition();
            auto declared = tag->getKindName().str() + body(*tag);
            return text + (is_using ? "using " + name + " = " + declared
                                    : "typedef " + declared + " " + name);
        }
        bool instance = instantiated(alias);
        auto shown = type_options(alias, alias.getDeclContext());
        auto type = instance ? member_type(alias.getUnderlyingType())
                             : alias.getTypeSourceInfo()->getType();
        if(is_using) {
            return text + "using " + name + " = " + declarator_type(type, "", shown);
        }
        return text + "typedef " + declarator_type(type, name, shown);
    }

    /// A function whose types read as a reader knows them, which clang's
    /// printer cannot do; the rest as clang prints it, but an instantiation's
    /// `explicit` and `noexcept` as what it made of them.
    auto function(const clang::FunctionDecl& function, llvm::ArrayRef<std::string> after)
        -> std::string {
        auto shown = type_options(function, function.getDeclContext());
        std::string text;
        if(function.isFunctionTemplateSpecialization()) {
            text += "template<> ";
        }
        if(function.getStorageClass() != clang::SC_None) {
            text += clang::VarDecl::getStorageClassSpecifierString(function.getStorageClass());
            text += ' ';
        }
        if(function.isInlineSpecified()) {
            text += "inline ";
        }
        if(function.isVirtualAsWritten()) {
            text += "virtual ";
        }
        if(function.isConstexprSpecified() && !function.isExplicitlyDefaulted()) {
            text += "constexpr ";
        }
        if(function.isConsteval()) {
            text += "consteval ";
        }
        /// A template's `explicit(condition)` shown through a dependent
        /// specialization stays undecided.
        if(auto specifier = clang::ExplicitSpecifier::getFromDecl(&function);
           specifier.isExplicit()) {
            text += "explicit ";
        } else if(specifier.getKind() == clang::ExplicitSpecKind::Unresolved) {
            llvm::raw_string_ostream specifier_os(text);
            specifier_os << "explicit(";
            specifier.getExpr()->printPretty(specifier_os, nullptr, policy, 0, "\n", &context);
            specifier_os << ") ";
        }

        std::string declarator;
        llvm::raw_string_ostream os(declarator);
        /// A conversion function's name is its type, and takes no template
        /// arguments.
        if(const auto* conversion = llvm::dyn_cast<clang::CXXConversionDecl>(&function)) {
            declarator +=
                "operator " + display::type(context, conversion->getConversionType(), shown).text;
        } else {
            function.getNameInfo().printName(os, policy);
            if(const auto* written = function.getTemplateSpecializationArgsAsWritten()) {
                clang::printTemplateArgumentList(os, written->arguments(), policy);
            } else if(const auto* args = function.getTemplateSpecializationArgs()) {
                /// A pack's arguments flattened, as the arguments of a call.
                llvm::SmallVector<std::string> arguments;
                for(const auto& arg: args->asArray()) {
                    for(const auto& element: arg.getKind() == clang::TemplateArgument::Pack
                                                 ? arg.pack_elements()
                                                 : llvm::ArrayRef(arg)) {
                        if(element.getKind() == clang::TemplateArgument::Type) {
                            arguments.push_back(
                                display::type(context, element.getAsType(), shown).text);
                        } else {
                            llvm::raw_string_ostream argument_os(arguments.emplace_back());
                            element.print(policy, argument_os, /*IncludeType=*/true);
                        }
                    }
                }
                declarator += "<" + llvm::join(arguments, ", ") + ">";
            }
        }

        llvm::SmallVector<std::string> parameters;
        for(const auto* param: function.parameters()) {
            auto name = policy.CleanUglifiedParameters && param->getIdentifier()
                            ? param->getIdentifier()->deuglifiedName()
                            : param->getName();
            auto declared = parameter(*param, name, shown);
            /// An instantiation's default argument is the template's, written
            /// with its parameters, until a call uses it.
            if(auto value = default_argument(param, shown);
               value && param->hasUninstantiatedDefaultArg()) {
                declared.resize(declared.size() - value->size() - llvm::StringRef(" = ").size());
            }
            parameters.push_back(param->isExplicitObjectParameter() ? "this " + declared
                                                                    : std::move(declared));
        }
        if(function.isVariadic()) {
            parameters.push_back("...");
        }
        declarator += "(" + llvm::join(parameters, ", ") + ")";
        if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
           method && !method->isExplicitObjectMemberFunction()) {
            declarator += method_qualifiers(*method);
        }
        const auto* prototype = function.getType()->castAs<clang::FunctionProtoType>();
        switch(prototype->getExceptionSpecType()) {
            case clang::EST_DynamicNone: declarator += " throw()"; break;
            case clang::EST_Dynamic: {
                llvm::SmallVector<std::string> types;
                for(auto type: prototype->exceptions()) {
                    types.push_back(display::type(context, type, shown).text);
                }
                declarator += " throw(" + llvm::join(types, ", ") + ")";
                break;
            }
            case clang::EST_BasicNoexcept:
            case clang::EST_NoexceptTrue: declarator += " noexcept"; break;
            case clang::EST_DependentNoexcept:
                declarator += " noexcept(";
                prototype->getNoexceptExpr()->printPretty(os, nullptr, policy, 0, "\n", &context);
                declarator += ")";
                break;
            default: break;
        }

        if(llvm::isa<clang::CXXConstructorDecl, clang::CXXDestructorDecl, clang::CXXConversionDecl>(
               function)) {
            text += declarator;
        } else if(prototype->hasTrailingReturn()) {
            text += "auto " + declarator + " -> " +
                    display::type(context, function.getReturnType(), shown).text;
        } else {
            text += display::type(context, function.getReturnType(), shown, declarator).text;
        }
        if(const auto* constraint = function.getTrailingRequiresClause().ConstraintExpr) {
            llvm::raw_string_ostream constraint_os(text);
            constraint_os << " requires ";
            constraint->printPretty(constraint_os, nullptr, policy, 0, "\n", &context);
        }
        for(const auto& attribute: after) {
            text += " " + attribute;
        }
        if(function.isPureVirtual()) {
            text += " = 0";
        } else if(function.isDeletedAsWritten()) {
            text += " = delete";
            if(const auto* message = function.getDeletedMessage()) {
                llvm::raw_string_ostream message_os(text);
                message_os << '(';
                message->outputString(message_os);
                message_os << ')';
            }
        } else if(function.isExplicitlyDefaulted()) {
            text += " = default";
        }
        return text;
    }

    auto template_head(const clang::TemplateParameterList& params) -> std::string {
        std::string text;
        llvm::raw_string_ostream os(text);
        params.print(os, context, policy);
        /// An abbreviated template's head is invented, and printed as none.
        if(!text.empty() && !llvm::StringRef(text).ends_with(" ")) {
            text += ' ';
        }
        return text;
    }

    /// The head of a class or an enum: its template head, key, attributes,
    /// name, arguments, `final` and bases; an enum's underlying type.
    auto tag_head(const clang::TagDecl& tag) -> std::string {
        std::string text;
        const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&tag);
        const auto* spec =
            llvm::dyn_cast_if_present<clang::ClassTemplateSpecializationDecl>(record);
        const auto* partial =
            llvm::dyn_cast_if_present<clang::ClassTemplatePartialSpecializationDecl>(record);
        bool instance =
            spec && !partial && spec->getSpecializationKind() != clang::TSK_ExplicitSpecialization;
        if(record && record->getDescribedClassTemplate()) {
            text += template_head(*record->getDescribedClassTemplate()->getTemplateParameters());
        } else if(partial) {
            text += template_head(*partial->getTemplateParameters());
        } else if(spec) {
            text += "template <> ";
        }
        text += tag.getKindName();
        if(const auto* enum_decl = llvm::dyn_cast<clang::EnumDecl>(&tag);
           enum_decl && enum_decl->isScoped()) {
            text += enum_decl->isScopedUsingClassTag() ? " class" : " struct";
        }
        auto attributes = written(tag);
        for(const auto& attribute: llvm::concat<std::string>(attributes.before, attributes.after)) {
            text += " " + attribute;
        }
        if(tag.getDeclName()) {
            text += ' ';
            if(auto qualifier = tag.getQualifier(); qualifier && !instance) {
                llvm::raw_string_ostream os(text);
                qualifier.print(os, policy);
            }
            text += tag.getName();
        }
        if(instance) {
            text += specialization_args(*spec, options);
        } else if(spec) {
            text += template_args(*spec);
        }
        if(tag.hasAttr<clang::FinalAttr>()) {
            text += " final";
        }
        if(const auto* enum_decl = llvm::dyn_cast<clang::EnumDecl>(&tag);
           enum_decl && enum_decl->isFixed()) {
            auto integer = enum_decl->getIntegerTypeSourceInfo()
                               ? enum_decl->getIntegerTypeSourceInfo()->getType()
                               : enum_decl->getIntegerType();
            text += " : " + display::type(context, integer, type_options(tag, &tag)).text;
        }
        if(!record || !record->hasDefinition()) {
            return text;
        }
        llvm::StringRef separator = " : ";
        auto shown = type_options(tag, tag.getDeclContext());
        if(instance) {
            shown.readable = options.readable;
        }
        for(const auto& base: record->bases()) {
            const auto* base_decl = base.getType()->getAsCXXRecordDecl();
            /// An instantiation's bases the library keeps to itself
            /// (`_Vector_base`) say nothing of it.
            if(instance && base_decl && options.readable && options.readable->internal(base_decl)) {
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
            auto type = !instance && base.getTypeSourceInfo() ? base.getTypeSourceInfo()->getType()
                                                              : base.getType();
            text += display::type(context, type, shown).text;
            if(base.isPackExpansion()) {
                text += "...";
            }
        }
        return text;
    }

    /// A named type a class declares, as its summary lists it: by name, as
    /// its own card shows its members — but an unscoped enum's
    /// enumerators, which are the class's members too and which C++ has no
    /// way to declare the enum without.
    auto mention(const clang::Decl& member) -> std::string {
        if(const auto* class_template = llvm::dyn_cast<clang::ClassTemplateDecl>(&member)) {
            const auto* record = class_template->getTemplatedDecl();
            return template_head(*class_template->getTemplateParameters()) +
                   record->getKindName().str() + " " + record->getName().str();
        }
        const auto& tag = llvm::cast<clang::TagDecl>(member);
        if(const auto* enum_decl = llvm::dyn_cast<clang::EnumDecl>(&tag)) {
            if(!enum_decl->isFixed() && enum_decl->getDefinition()) {
                return tag_head(*enum_decl) + body(*enum_decl->getDefinition());
            }
            return tag_head(*enum_decl);
        }
        return tag.getKindName().str() + " " + tag.getName().str();
    }

    auto body(const clang::TagDecl& tag) -> std::string {
        std::string text = " {";
        if(const auto* enum_decl = llvm::dyn_cast<clang::EnumDecl>(&tag)) {
            enumerators(*enum_decl, text);
        } else if(tag.isCompleteDefinition()) {
            members(llvm::cast<clang::RecordDecl>(tag), text);
        } else {
            /// An instantiation defines a member class only once it is used.
            text += "\n// ...";
        }
        return text + "\n}";
    }

    void enumerators(const clang::EnumDecl& decl, std::string& text) {
        bool instance = instantiated(decl);
        llvm::StringRef separator;
        for(const clang::EnumConstantDecl* enumerator: decl.enumerators()) {
            text += separator;
            separator = ",";
            if(!fits(text)) {
                return;
            }
            text += '\n';
            text += enumerator->getName();
            if(auto attributes = written(*enumerator); !attributes.after.empty()) {
                text += " " + join(attributes.after);
            }
            if(const clang::Expr* init = enumerator->getInitExpr()) {
                if(auto value = initializer(init, instance)) {
                    text += " = " + *value;
                    continue;
                }
            }
            if(!enumerator->getType()->isDependentType()) {
                text += " = " + llvm::toString(enumerator->getInitVal(), 10);
            }
        }
    }

    void members(const clang::RecordDecl& record, std::string& text) {
        auto access = record.isClass() ? clang::AS_private : clang::AS_public;
        auto decls = source_order(record);
        bool listed = false;
        bool hidden = false;
        for(auto it = decls.begin(); it != decls.end(); ++it) {
            const clang::Decl* member = *it;
            if(!is_listed(*member)) {
                hidden |= is_reserved(*member) && llvm::isa<clang::FieldDecl>(member);
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
            if(!fits(text)) {
                return;
            }
            listed = true;

            /// C has no access control: its members have AS_none.
            if(member->getAccess() != access && member->getAccess() != clang::AS_none) {
                access = member->getAccess();
                text += '\n';
                text += clang::getAccessSpelling(access);
                text += ':';
            }
            text += '\n';

            if(tag && !declarators.empty()) {
                /// The first declarator writes the type, the tag's members
                /// with it when it has no name of its own.
                text += declaration(*declarators.front());
                for(const auto* declarator: llvm::drop_begin(declarators)) {
                    text += ", ";
                    text += later(*declarator);
                }
                it = last;
            } else if(tag && !tag->getDeclName()) {
                text += tag->getKindName().str() + body(*tag);
            } else if(llvm::isa<clang::TagDecl, clang::ClassTemplateDecl>(member)) {
                text += mention(*member);
            } else {
                text += declaration(*member);
            }
            text += ';';
        }
        /// A class whose data all is the implementation's own is no empty
        /// one.
        if(!listed && hidden) {
            text += "\n// ...";
        }
    }

    /// A declarator after the first: `*b` in `struct { ... } a, *b;`.
    auto later(const clang::Decl& declarator) -> std::string {
        auto shown = type_options(declarator, declarator.getDeclContext());
        const auto& named = llvm::cast<clang::NamedDecl>(declarator);
        clang::QualType type;
        if(const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(&declarator)) {
            type = alias->getTypeSourceInfo()->getType();
        } else {
            const auto& value = llvm::cast<clang::DeclaratorDecl>(declarator);
            type = instantiated(declarator) || !value.getTypeSourceInfo()
                       ? value.getType()
                       : value.getTypeSourceInfo()->getType();
        }
        auto text = later_declarator(type, named.getName(), shown);
        if(const auto* field = llvm::dyn_cast<clang::FieldDecl>(&declarator)) {
            if(const auto* init = field->getInClassInitializer()) {
                if(auto written = initializer(init, instantiated(declarator))) {
                    text += field->getInClassInitStyle() == clang::ICIS_ListInit ? " " : " = ";
                    text += *written;
                }
            }
        } else if(const auto* var = llvm::dyn_cast<clang::VarDecl>(&declarator)) {
            text += variable_initializer(*var, instantiated(declarator));
        }
        return text;
    }

    /// A data member, an alias or a variable, as its declaration writes it.
    auto declaration(const clang::Decl& decl) -> std::string {
        if(const auto* var_template = llvm::dyn_cast<clang::VarTemplateDecl>(&decl)) {
            return template_head(*var_template->getTemplateParameters()) +
                   variable(*var_template->getTemplatedDecl());
        }
        if(const auto* alias_template = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(&decl)) {
            return template_head(*alias_template->getTemplateParameters()) +
                   alias(*alias_template->getTemplatedDecl());
        }
        if(const auto* var = llvm::dyn_cast<clang::VarDecl>(&decl)) {
            return variable(*var);
        }
        if(const auto* field_decl = llvm::dyn_cast<clang::FieldDecl>(&decl)) {
            return field(*field_decl);
        }
        return alias(llvm::cast<clang::TypedefNameDecl>(decl));
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

    if(printed == name) {
        return;
    }
    /// The name, not a return type it begins (`Buffer::size_type Buffer::size()`).
    for(auto at = text.find(printed); at != std::string::npos; at = text.find(printed, at + 1)) {
        auto end = at + printed.size();
        if(end == text.size() || !clang::isAsciiIdentifierContinue(text[end])) {
            text.replace(at, printed.size(), name);
            return;
        }
    }
}

/// A function as clang prints it, its attributes as the source writes them:
/// those after its declarator before a `= 0`, `= delete` or `= default`.
auto function_text(const clang::FunctionDecl& function,
                   clang::PrintingPolicy policy,
                   llvm::ArrayRef<std::string> after) -> std::string {
    /// A declaration no code writes has no types written either: the
    /// global `operator new` takes the compiler's own `__size_t`.
    if(function.isImplicit()) {
        policy.PrintAsCanonical = true;
    }
    std::string text;
    llvm::raw_string_ostream os(text);
    function.print(os, policy);
    print_name(text, function, policy);
    if(!after.empty()) {
        std::size_t suffix = text.size();
        if(function.isPureVirtual()) {
            suffix = text.rfind(" = 0");
        } else if(function.isDeletedAsWritten()) {
            suffix = text.rfind(" = delete");
        } else if(function.isExplicitlyDefaulted()) {
            suffix = text.rfind(" = default");
        }
        if(suffix == std::string::npos) {
            suffix = text.size();
        }
        text.insert(suffix, " " + join(after));
    }
    return text;
}

}  // namespace

auto attribute(const clang::Attr& attr, const clang::ASTContext& context) -> std::string {
    auto& sources = context.getSourceManager();
    std::string written;
    /// A keyword's range is the keyword alone (`alignas`) or its argument
    /// alone (`asm("name")`'s); clang prints keywords as written.
    if(!attr.isKeywordAttribute()) {
        auto range =
            clang::Lexer::makeFileCharRange(clang::CharSourceRange::getTokenRange(attr.getRange()),
                                            sources,
                                            context.getLangOpts());
        if(range.isValid()) {
            written = clang::Lexer::getSourceText(range, sources, context.getLangOpts()).str();
        }
    }
    if(written.empty()) {
        return llvm::StringRef(pretty(attr, context.getPrintingPolicy())).trim().str();
    }
    if(attr.isCXX11Attribute() || attr.isC23Attribute()) {
        /// `[[using gnu: hot, aligned(8)]]` writes the scope once, before the
        /// list, where the range of an attribute may start: the attribute
        /// is its last name outside parentheses on.
        if(attr.hasScope() && attr.getAttrName()) {
            llvm::StringRef text = written;
            auto name = attr.getAttrName()->getName();
            auto start = llvm::StringRef::npos;
            int depth = 0;
            for(std::size_t index = 0; index < text.size(); index += 1) {
                if(text[index] == '(') {
                    depth += 1;
                } else if(text[index] == ')') {
                    depth -= 1;
                } else if(depth == 0 && text.substr(index).starts_with(name) &&
                          (index == 0 || !clang::isAsciiIdentifierContinue(text[index - 1])) &&
                          (index + name.size() == text.size() ||
                           !clang::isAsciiIdentifierContinue(text[index + name.size()]))) {
                    start = index;
                }
            }
            if(start != llvm::StringRef::npos) {
                written = (attr.getScopeName()->getName() + "::" + text.substr(start)).str();
            }
        }
        return "[[" + written + "]]";
    }
    if(attr.isGNUAttribute()) {
        return "__attribute__((" + written + "))";
    }
    if(attr.isDeclspecAttribute()) {
        return "__declspec(" + written + ")";
    }
    return written;
}

auto definition(const clang::Decl* decl,
                const Options& options,
                llvm::function_ref<std::size_t(clang::SourceRange)> token_count) -> Definition {
    assert(decl);
    auto& context = decl->getASTContext();
    auto written = options;
    written.injected_as_written = true;
    clang::PrintingPolicy policy = printing_policy(context, written);
    EnclosingScopes scopes(decl->getDeclContext());
    policy.Callbacks = &scopes;
    /// Attributes print as the source writes them, which clang's printer
    /// does not know.
    policy.SuppressDeclAttributes = true;

    Definition result;
    DeclarationPrinter printer{
        .context = context,
        .policy = policy,
        .options = options,
        .token_count = token_count,
        .budget = options.max_members,
        .macros = result.attribute_macros,
    };
    const auto* templated = decl;
    if(const auto* described = llvm::dyn_cast<clang::TemplateDecl>(decl);
       described && described->getTemplatedDecl() &&
       !llvm::isa<clang::ClassTemplateDecl, clang::ConceptDecl>(described)) {
        result.head = printer.template_head(*described->getTemplateParameters());
        templated = described->getTemplatedDecl();
    }
    /// Attributes written before a declaration go on a line of their own;
    /// a class's and an enum's come after its key.
    if(!llvm::isa<clang::TagDecl, clang::ClassTemplateDecl, clang::ParmVarDecl>(templated)) {
        printer.apart = templated;
        result.attributes = join(printer.written(*templated).before);
    }

    const auto* class_template = llvm::dyn_cast<clang::ClassTemplateDecl>(decl);
    const auto* tag =
        llvm::dyn_cast<clang::TagDecl>(class_template ? class_template->getTemplatedDecl() : decl);
    const auto* var = llvm::dyn_cast<clang::VarDecl>(templated);
    std::string& text = result.text;
    llvm::raw_string_ostream os(text);
    if(const auto* decomposition = llvm::dyn_cast<clang::DecompositionDecl>(decl)) {
        print_decomposition(*decomposition, policy, os);
    } else if(const auto* param = llvm::dyn_cast<clang::ParmVarDecl>(decl)) {
        /// Its attributes and storage class are no part of its type.
        auto attributes = printer.written(*param);
        for(const auto& attribute: attributes.before) {
            os << attribute << ' ';
        }
        if(param->getStorageClass() == clang::SC_Register) {
            os << "register ";
        }
        if(param->isExplicitObjectParameter()) {
            os << "this ";
        }
        /// As written, as clang prints every other declaration.
        auto written = options;
        written.resolve_decltype = false;
        written.readable = nullptr;
        std::string name = param->getName().str();
        if(!attributes.after.empty()) {
            name += " " + join(attributes.after);
        }
        auto declared = parameter(*param, name, written);
        /// A redeclaration inherits the default argument it does not write.
        if(auto value = default_argument(param, written);
           value && param->hasInheritedDefaultArg()) {
            declared.resize(declared.size() - value->size() - llvm::StringRef(" = ").size());
        }
        os << declared;
    } else if(var && var->isInitCapture()) {
        /// Clang prints the capture as a variable of its deduced type,
        /// `auto` left in front.
        auto type = var->getType();
        if(const auto* expansion = type->getAs<clang::PackExpansionType>()) {
            type = expansion->getPattern();
        }
        os << "auto " << (type->isReferenceType() ? "&" : "")
           << (var->isParameterPack() ? "..." : "") << var->getName();
        if(var->getInit() && !hides_initializer(var->getInit(), policy, token_count)) {
            if(auto init = as_written(context, var->getInit(), options)) {
                switch(var->getInitStyle()) {
                    case clang::VarDecl::CInit: os << " = " << *init; break;
                    case clang::VarDecl::CallInit: os << "(" << *init << ")"; break;
                    case clang::VarDecl::ListInit:
                    case clang::VarDecl::ParenListInit: os << *init; break;
                }
            }
        }
    } else if(llvm::isa<clang::VarDecl, clang::FieldDecl, clang::TypedefNameDecl>(templated)) {
        os << printer.declaration(*templated);
    } else if(tag) {
        /// The head comes from the definition too: only a definition has
        /// its bases.
        const auto* definition = tag->getDefinition();
        os << printer.tag_head(definition ? *definition : *tag);
        if(definition && options.max_members > 0) {
            os << printer.body(*definition);
        }
    } else if(const auto* function = templated->getAsFunction()) {
        auto after = printer.written(*function).after;
        /// GNU's `noreturn` marks the function's type, and leaves no
        /// attribute on the declaration.
        if(function->getType()->castAs<clang::FunctionType>()->getNoReturnAttr() &&
           !function->hasAttr<clang::C11NoReturnAttr>() &&
           !function->hasAttr<clang::CXX11NoReturnAttr>()) {
            after.push_back("__attribute__((noreturn))");
        }
        /// Clang prints an instantiation's types as it made them: a `T` the
        /// arguments' canonical type, `basic_string<char>` for `string`.
        if(printer.type_options(*function, function->getDeclContext()).readable) {
            os << printer.function(*function, after);
        } else {
            os << function_text(*function, policy, after);
        }
        /// A specialization's `template <>` heads it, before its attributes.
        if(llvm::StringRef(text).starts_with("template<> ")) {
            result.head = "template <> ";
            text.erase(0, llvm::StringRef("template<> ").size());
        }
    } else {
        decl->print(os, policy);
    }

    /// The declarations of a private module fragment are module private,
    /// which clang prints as the `__module_private__` specifier of its own
    /// module maps: no C++20 code spells it.
    if(decl->isModulePrivate()) {
        constexpr llvm::StringLiteral specifier = "__module_private__ ";
        for(auto at = text.find(specifier); at != std::string::npos;
            at = text.find(specifier, at)) {
            text.erase(at, specifier.size());
        }
    }
    return result;
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

namespace {

/// The instantiation of `pattern`, a class template's or a member class of
/// one, that `scope` is in.
auto instance_around(const clang::CXXRecordDecl& pattern, const clang::DeclContext* scope)
    -> const clang::CXXRecordDecl* {
    for(; scope; scope = scope->getParent()) {
        const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(scope);
        const auto* instantiated = record ? record->getTemplateInstantiationPattern() : nullptr;
        if(instantiated && instantiated->getCanonicalDecl() == pattern.getCanonicalDecl()) {
            return record;
        }
    }
    return nullptr;
}

/// The instantiation of `pattern` that a member `expr` names belongs to.
auto instance_named(const clang::CXXRecordDecl& pattern, const clang::Stmt& expr)
    -> const clang::CXXRecordDecl* {
    const clang::Decl* named = nullptr;
    if(const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(&expr)) {
        named = reference->getDecl();
    } else if(const auto* member = llvm::dyn_cast<clang::MemberExpr>(&expr)) {
        named = member->getMemberDecl();
    }
    if(const auto* found = named ? instance_around(pattern, named->getDeclContext()) : nullptr) {
        return found;
    }
    for(const auto* child: expr.children()) {
        if(const auto* found = child ? instance_named(pattern, *child) : nullptr) {
            return found;
        }
    }
    return nullptr;
}

/// Whether a using-declaration in the namespace around `ns` brings `decl`,
/// a member of `ns`, out of it.
bool exported(const clang::NamedDecl& decl, const clang::NamespaceDecl& ns) {
    return llvm::any_of(ns.getParent()->lookup(decl.getDeclName()),
                        [&](const clang::NamedDecl* found) {
                            const auto* shadow = llvm::dyn_cast<clang::UsingShadowDecl>(found);
                            return shadow && shadow->getTargetDecl()->getCanonicalDecl() ==
                                                 decl.getCanonicalDecl();
                        });
}

/// Whether `name` in `derived`, a class derived from `record`, finds
/// `record`'s member to code outside them: `record` is a public base, and no
/// class on the way declares its own.
bool inherits(const clang::CXXRecordDecl& derived,
              const clang::CXXRecordDecl& record,
              clang::DeclarationName name) {
    clang::CXXBasePaths paths;
    if(!derived.isDerivedFrom(&record, paths) || !derived.lookup(name).empty()) {
        return false;
    }
    return llvm::all_of(paths, [&](const clang::CXXBasePath& path) {
        return path.Access == clang::AS_public &&
               llvm::all_of(path, [&](const clang::CXXBasePathElement& element) {
                   const auto* base = element.Base->getType()->getAsCXXRecordDecl();
                   return base->getCanonicalDecl() == record.getCanonicalDecl() ||
                          base->lookup(name).empty();
               });
    });
}

/// The class derived from `record` through which a member access in `expr`
/// reaches a member of `record`, where `name` is still `record`'s: `DenseMap`
/// for `map.find(key)`, `find` being its base `DenseMapBase`'s.
auto accessed_through(const clang::CXXRecordDecl& record,
                      clang::DeclarationName name,
                      const clang::Stmt& expr) -> const clang::CXXRecordDecl* {
    if(const auto* member = llvm::dyn_cast<clang::MemberExpr>(&expr)) {
        const auto* owner =
            llvm::dyn_cast<clang::CXXRecordDecl>(member->getMemberDecl()->getDeclContext());
        auto object = member->getBase()->IgnoreParenBaseCasts()->getType();
        if(member->isArrow()) {
            object = object->getPointeeType();
        }
        const auto* naming = object.isNull() ? nullptr : object->getAsCXXRecordDecl();
        if(owner && naming && owner->getCanonicalDecl() == record.getCanonicalDecl() &&
           naming->getCanonicalDecl() != record.getCanonicalDecl() && naming->hasDefinition() &&
           inherits(*naming, record, name)) {
            return naming;
        }
    }
    for(const auto* child: expr.children()) {
        if(const auto* found = child ? accessed_through(record, name, *child) : nullptr) {
            return found;
        }
    }
    return nullptr;
}

bool mentions(const clang::CXXRecordDecl& record, const clang::Type* type);

/// Whether `within` spells `type`, a canonical one: is it, or has it among
/// its parts or a class's template arguments.
bool mentions(clang::QualType within, const clang::Type* type) {
    const auto* canonical = within.getCanonicalType().getTypePtr();
    if(canonical == type) {
        return true;
    }
    if(auto pointee = canonical->getPointeeType(); !pointee.isNull()) {
        return mentions(pointee, type);
    }
    if(const auto* array = llvm::dyn_cast<clang::ArrayType>(canonical)) {
        return mentions(array->getElementType(), type);
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionProtoType>(canonical)) {
        return mentions(function->getReturnType(), type) ||
               llvm::any_of(function->param_types(),
                            [&](clang::QualType param) { return mentions(param, type); });
    }
    const auto* record = canonical->getAsCXXRecordDecl();
    return record && mentions(*record, type);
}

bool mentions(const clang::TemplateArgument& argument, const clang::Type* type) {
    if(argument.getKind() == clang::TemplateArgument::Pack) {
        return llvm::any_of(argument.pack_elements(), [&](const clang::TemplateArgument& element) {
            return mentions(element, type);
        });
    }
    return argument.getKind() == clang::TemplateArgument::Type &&
           mentions(argument.getAsType(), type);
}

/// Whether naming `record` spells `type`, a canonical one, among its or its
/// enclosing classes' template arguments.
bool mentions(const clang::CXXRecordDecl& record, const clang::Type* type) {
    for(const auto* scope = &record; scope;
        scope = llvm::dyn_cast<clang::CXXRecordDecl>(scope->getDeclContext())) {
        const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(scope);
        if(spec && llvm::any_of(spec->getTemplateArgs().asArray(),
                                [&](const clang::TemplateArgument& argument) {
                                    return mentions(argument, type);
                                })) {
            return true;
        }
    }
    return false;
}

/// Whether a using-directive of a namespace or a class around `scope`
/// nominates `ns`. One in a function body is no declaration of its
/// context: Sema keeps it in the block's scope alone.
bool nominated(const clang::NamespaceDecl& ns, const clang::DeclContext* scope) {
    for(; scope; scope = scope->getParent()) {
        for(const auto* directive: scope->using_directives()) {
            if(directive->getNominatedNamespace()->getCanonicalDecl() == ns.getCanonicalDecl()) {
                return true;
            }
        }
    }
    return false;
}

/// Whether `alias` computes its type as a trait does, rather than naming it:
/// the member `type` of a class template specialization
/// (`common_type<A, B>::type`).
bool computes(const clang::TypedefNameDecl& alias) {
    const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(alias.getDeclContext());
    return record && alias.getName() == "type" &&
           (llvm::isa<clang::ClassTemplateSpecializationDecl>(record) ||
            record->getTemplateInstantiationPattern());
}

/// Whether the alias template `alias` computes its type as a trait does:
/// written as a computation (`enable_if_t`), or as an alias template that
/// computes or is the library's own (`iterator_t` over `__range_iter_t`);
/// not as the class it names (`pmr::vector`) or another alias naming one.
bool computes(const clang::TypeAliasTemplateDecl& alias) {
    const auto* pattern = alias.getTemplatedDecl()->getUnderlyingType().getTypePtr();
    if(const auto* TST = llvm::dyn_cast<clang::TemplateSpecializationType>(pattern)) {
        const auto* target = llvm::dyn_cast_if_present<clang::TypeAliasTemplateDecl>(
            TST->getTemplateName().getAsTemplateDecl());
        return target && (clang::isReservedInAllContexts(
                              target->isReserved(alias.getASTContext().getLangOpts())) ||
                          computes(*target));
    }
    return llvm::isa<clang::DependentNameType,
                     clang::DecltypeType,
                     clang::UnaryTransformType,
                     clang::PackIndexingType>(pattern);
}

/// Whether `alias` is a member of a class template, named through a
/// specialization that spells its arguments again
/// (`vector<int>::size_type`). Its specializations share one that names the
/// same type in all of them.
bool specialization_member(const clang::TypedefNameDecl& alias) {
    const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(alias.getDeclContext());
    return record && (llvm::isa<clang::ClassTemplateSpecializationDecl>(record) ||
                      record->getTemplateInstantiationPattern() || record->isTemplated());
}

}  // namespace

auto ReadableTypes::qualifier(const clang::NamedDecl& decl,
                              const clang::DeclContext* scope,
                              const clang::Expr* source) -> clang::NestedNameSpecifier {
    const clang::DeclContext* shown = scope ? scope : context.getTranslationUnitDecl();
    /// A class's members are found unqualified in the classes derived from
    /// it.
    auto found = [&](const clang::CXXRecordDecl& record) {
        for(const auto* outer = shown; outer; outer = outer->getParent()) {
            const auto* derived = llvm::dyn_cast<clang::CXXRecordDecl>(outer);
            if(derived && derived->hasDefinition() && derived->isDerivedFrom(&record)) {
                return true;
            }
        }
        return false;
    };
    // The namespaces code names it through, up to one it is shown in: an
    // inline or an anonymous one is not written; a class names its own.
    llvm::SmallVector<const clang::NamespaceDecl*> namespaces;
    /// The declaration `parent` declares on the way out.
    const auto* member = &decl;
    for(const auto* parent = decl.getDeclContext(); !parent->Encloses(shown);
        parent = parent->getParent()) {
        if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(parent)) {
            /// A class template's member that names the same type in every
            /// specialization stays the template's own in them, whose members
            /// keep the types the template writes: it is found unqualified in
            /// a specialization, and named through the one `source` names a
            /// member of; by its name alone without one.
            if(record->isDependentContext()) {
                if(instance_around(*record, shown)) {
                    return std::nullopt;
                }
                record = source ? instance_named(*record, *source) : nullptr;
                if(!record) {
                    return std::nullopt;
                }
            } else if(const auto* through =
                          source && !found(*record)
                              ? accessed_through(*record, decl.getDeclName(), *source)
                              : nullptr) {
                record = through;
            }
            if(found(*record)) {
                return std::nullopt;
            }
            auto type =
                readable(context.getCanonicalTagType(record), scope, source, Sugar::Written);
            return clang::NestedNameSpecifier(type.getTypePtr());
        }
        const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(parent);
        // A function's own types are named nowhere else.
        if(!ns && !llvm::isa<clang::LinkageSpecDecl, clang::ExportDecl>(parent)) {
            return std::nullopt;
        }
        /// One of the library's own that a using-declaration brings it out
        /// of names it no more than an inline one.
        if(ns && !ns->isInline() && !ns->isAnonymousNamespace() &&
           !(reserved(ns) && exported(*member, *ns))) {
            namespaces.push_back(ns);
        }
        if(ns) {
            member = ns;
        }
    }
    // A hint drops the outermost ones from the innermost code there sees
    // without naming it: `std`, or one a using-directive brings in; short
    // names keep only those inside `std`.
    if(hinted || !scoped) {
        auto seen = llvm::find_if(namespaces, [&](const clang::NamespaceDecl* ns) {
            return ns->isStdNamespace() || (hinted && nominated(*ns, shown));
        });
        bool in_std = seen != namespaces.end() && (*seen)->isStdNamespace();
        namespaces.erase(seen, namespaces.end());
        if(!hinted && !in_std) {
            namespaces.clear();
        }
    }
    clang::NestedNameSpecifier result = std::nullopt;
    for(const auto* ns: llvm::reverse(namespaces)) {
        result = clang::NestedNameSpecifier(context, ns, result);
    }
    return result;
}

bool ReadableTypes::reserved(const clang::NamedDecl* decl) const {
    return decl && clang::isReservedInAllContexts(decl->isReserved(context.getLangOpts()));
}

bool ReadableTypes::internal(const clang::NamedDecl* decl) const {
    if(!decl) {
        return false;
    }
    bool hidden = reserved(decl);
    /// The declaration `scope` declares on the way out.
    const auto* member = decl;
    for(const auto* scope = decl->getDeclContext(); scope; scope = scope->getParent()) {
        const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(scope);
        hidden |= record && reserved(record);
        // libc++'s `std::__1` and libstdc++'s `std::__cxx11` hide nothing,
        // nor does a namespace a using-declaration brings a class out of
        // (libstdc++'s `std::__exception_ptr::exception_ptr`).
        if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(scope);
           ns && (ns->isStdNamespace() || reserved(ns))) {
            return hidden || (reserved(ns) && !ns->isInline() && !exported(*member, *ns));
        }
        if(const auto* named = llvm::dyn_cast<clang::NamedDecl>(scope)) {
            member = named;
        }
    }
    return false;
}

bool ReadableTypes::internal(clang::QualType type) const {
    while(!type.isNull()) {
        const auto* T = type.getTypePtr();
        if(const auto* alias = llvm::dyn_cast<clang::TypedefType>(T)) {
            return internal(alias->getDecl());
        }
        if(const auto* TST = llvm::dyn_cast<clang::TemplateSpecializationType>(T)) {
            return internal(TST->getTemplateName().getAsTemplateDecl());
        }
        if(const auto* tag = llvm::dyn_cast<clang::TagType>(T)) {
            return internal(tag->getDecl());
        }
        if(const auto* array = llvm::dyn_cast<clang::ArrayType>(T)) {
            type = array->getElementType();
        } else if(const auto* paren = llvm::dyn_cast<clang::ParenType>(T)) {
            type = paren->getInnerType();
        } else {
            type = T->getPointeeType();
        }
    }
    return false;
}

bool ReadableTypes::shows_internal(clang::QualType type) const {
    if(type.isNull()) {
        return false;
    }
    auto through = [&](clang::NestedNameSpecifier qualifier) {
        return qualifier.getKind() == clang::NestedNameSpecifier::Kind::Type &&
               shows_internal(clang::QualType(qualifier.getAsType(), 0));
    };
    const auto* T = type.getTypePtr();
    if(const auto* alias = llvm::dyn_cast<clang::TypedefType>(T)) {
        return internal(alias->getDecl()) || through(alias->getQualifier());
    }
    if(const auto* tag = llvm::dyn_cast<clang::TagType>(T)) {
        return internal(tag->getDecl()) || through(tag->getQualifier());
    }
    if(const auto* TST = llvm::dyn_cast<clang::TemplateSpecializationType>(T)) {
        const auto* qualified = TST->getTemplateName().getAsQualifiedTemplateName();
        return internal(TST->getTemplateName().getAsTemplateDecl()) ||
               (qualified && through(qualified->getQualifier())) ||
               llvm::any_of(TST->template_arguments(),
                            [&](const clang::TemplateArgument& argument) {
                                return argument.getKind() == clang::TemplateArgument::Type &&
                                       shows_internal(argument.getAsType());
                            });
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionProtoType>(T)) {
        return shows_internal(function->getReturnType()) ||
               llvm::any_of(function->getParamTypes(),
                            [&](clang::QualType param) { return shows_internal(param); });
    }
    if(const auto* array = llvm::dyn_cast<clang::ArrayType>(T)) {
        return shows_internal(array->getElementType());
    }
    if(const auto* paren = llvm::dyn_cast<clang::ParenType>(T)) {
        return shows_internal(paren->getInnerType());
    }
    return shows_internal(T->getPointeeType());
}

auto ReadableTypes::public_alias(const clang::TagDecl& tag, const clang::Expr* source) const
    -> const clang::TypedefNameDecl* {
    auto type = context.getCanonicalTagType(&tag);
    llvm::SmallVector<const clang::CXXRecordDecl*, 4> classes;
    auto add = [&](clang::QualType candidate) {
        if(candidate.isNull() || candidate->isDependentType()) {
            return;
        }
        candidate = candidate.getNonReferenceType();
        if(candidate->isPointerType()) {
            candidate = candidate->getPointeeType();
        }
        // Naming the tag through a class that spells it in its arguments
        // (`reverse_iterator<T>::iterator_type`) would need its name first.
        const auto* record = candidate->getAsCXXRecordDecl();
        if(record && record->hasDefinition() && !internal(record) &&
           !llvm::is_contained(classes, record) && !mentions(*record, type.getTypePtr())) {
            classes.push_back(record);
        }
    };
    if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&tag)) {
        for(const auto& argument: spec->getTemplateArgs().asArray()) {
            if(argument.getKind() == clang::TemplateArgument::Type) {
                add(argument.getAsType());
            }
        }
    }
    llvm::SmallVector<const clang::Stmt*, 16> pending;
    if(source) {
        pending.push_back(source);
    }
    while(!pending.empty()) {
        const auto* stmt = pending.pop_back_val();
        if(const auto* expr = llvm::dyn_cast<clang::Expr>(stmt)) {
            add(expr->getType());
        }
        // A lambda's body is no part of what it is passed to.
        if(!llvm::isa<clang::LambdaExpr>(stmt)) {
            for(const auto* child: stmt->children()) {
                if(child) {
                    pending.push_back(child);
                }
            }
        }
    }
    for(const auto* record: classes) {
        const clang::TypedefNameDecl* found = nullptr;
        for(const auto* member: record->decls()) {
            const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(member);
            if(!alias || reserved(alias) || alias->getAccess() != clang::AS_public ||
               !context.hasSameType(alias->getUnderlyingType(), type)) {
                continue;
            }
            /// An alias written as another names a role of it: libc++'s
            /// `vector<bool>::iterator` is its `pointer`.
            const auto* restated =
                llvm::dyn_cast<clang::TypedefType>(alias->getUnderlyingType().getTypePtr());
            if(!found || (restated && restated->getDecl() == found)) {
                found = alias;
            }
        }
        if(found) {
            return found;
        }
    }
    return nullptr;
}

namespace {

/// Whether `expr` names what only the template it is written in has: a
/// parameter an instantiation substituted into it, or a member of its class
/// named without the class (`num` of `ratio<num, den>`).
bool template_bound(const clang::Stmt& expr) {
    if(llvm::isa<clang::SubstNonTypeTemplateParmExpr,
                 clang::SubstNonTypeTemplateParmPackExpr,
                 clang::SizeOfPackExpr>(expr)) {
        return true;
    }
    if(const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(&expr);
       reference && !reference->hasQualifier() &&
       reference->getDecl()->getDeclContext()->isRecord()) {
        return true;
    }
    return llvm::any_of(expr.children(),
                        [](const clang::Stmt* child) { return child && template_bound(*child); });
}

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
    const auto* outermost = decls::outermost_namespace(&spec);
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

auto ReadableTypes::argument_of(const clang::Type& type) const -> const clang::TemplateArgument* {
    const auto* param = llvm::dyn_cast<clang::TemplateTypeParmType>(&type);
    const auto* pattern =
        substitution ? substitution->getTemplateName().getAsTemplateDecl() : nullptr;
    if(!param || !pattern || !param->getDecl()) {
        return nullptr;
    }
    auto index = param->getIndex();
    const auto arguments = substitution->template_arguments();
    /// A pack's arguments are many, and an expansion among them stands for
    /// many: the template's own name stays.
    if(param->isParameterPack() ||
       llvm::any_of(arguments.take_front(index + 1), [](const clang::TemplateArgument& argument) {
           return argument.isPackExpansion();
       })) {
        return nullptr;
    }
    /// The parameter of any of its declarations: libstdc++ declares `map`
    /// again after its definition, whose members name the definition's.
    auto declares = [&](const clang::TemplateDecl& decl) {
        const auto* params = decl.getTemplateParameters();
        return param->getDepth() == params->getDepth() && index < params->size() &&
               params->getParam(index) == param->getDecl();
    };
    const auto* redeclarable = llvm::dyn_cast<clang::RedeclarableTemplateDecl>(pattern);
    bool own = redeclarable ? llvm::any_of(redeclarable->redecls(),
                                           [&](const clang::RedeclarableTemplateDecl* redecl) {
                                               return declares(*redecl);
                                           })
                            : declares(*pattern);
    if(!own || index >= arguments.size()) {
        return nullptr;
    }
    const auto& argument = arguments[index];
    return argument.getKind() == clang::TemplateArgument::Type ? &argument : nullptr;
}

auto ReadableTypes::readable(clang::QualType type,
                             const clang::DeclContext* scope,
                             const clang::Expr* source,
                             const clang::TemplateSpecializationType* specialization)
    -> clang::QualType {
    llvm::SaveAndRestore read(substitution, specialization);
    return readable(type, scope, source, hinted ? Sugar::Library : Sugar::Written);
}

auto ReadableTypes::underlying(clang::QualType type,
                               const clang::DeclContext* scope,
                               const clang::TemplateSpecializationType* specialization)
    -> clang::QualType {
    llvm::SaveAndRestore keep(aliasing, false);
    llvm::SaveAndRestore read(substitution, specialization);
    return readable(type, scope, nullptr, Sugar::Library);
}

auto ReadableTypes::readable(clang::QualType type,
                             const clang::DeclContext* scope,
                             const clang::Expr* source,
                             Sugar sugar) -> clang::QualType {
    /// A reference takes no qualifiers: a `const T` whose `T` is `U&` is a
    /// `U&`.
    auto qualified = [&](clang::QualType inner) {
        return inner->isReferenceType()
                   ? inner
                   : context.getQualifiedType(inner, type.getLocalQualifiers());
    };
    auto same = [&](clang::QualType inner) {
        return readable(inner, scope, source, sugar);
    };
    auto deduced = [&](clang::QualType inner) {
        return qualified(readable(inner, scope, source, std::max(sugar, Sugar::Deduced)));
    };
    auto beneath = [&](clang::QualType inner) {
        return qualified(readable(inner, scope, source, Sugar::Library));
    };
    /// Whether a name gets `named`, the qualifier code at the scope writes
    /// for it: one it needs none for keeps its own unless names are short.
    auto renamed = [&](clang::NestedNameSpecifier named) {
        return named || !scoped;
    };
    const auto* T = type.getTypePtr();
    /// The compiler's own names for the type of `sizeof` and of a pointer
    /// difference (`__size_t`), which no code spells.
    if(const auto* predefined = llvm::dyn_cast<clang::PredefinedSugarType>(T)) {
        if(const auto* alias = standard_name(*predefined)) {
            return qualified(context.getTypedefType(clang::ElaboratedTypeKeyword::None,
                                                    qualifier(*alias, scope, source),
                                                    alias));
        }
        return qualified(predefined->desugar());
    }
    if(const auto* placeholder = llvm::dyn_cast<clang::DeducedType>(T);
       placeholder && placeholder->getDeducedKind() == clang::DeducedKind::Deduced) {
        return deduced(placeholder->getDeducedType());
    }
    if(const auto* computed = llvm::dyn_cast<clang::DecltypeType>(T);
       computed && !computed->isDependentType() && sugar != Sugar::Written) {
        return deduced(computed->getUnderlyingType());
    }
    if(const auto* subst = llvm::dyn_cast<clang::SubstTemplateTypeParmType>(T)) {
        return qualified(same(subst->getReplacementType()));
    }
    /// The argument is the code's type, which the substitution does not reach
    /// again: `_Tp` of `vector<vector<T>>` reads `vector<T>`, whose `T` is
    /// the code's own.
    if(const auto* argument = argument_of(*T)) {
        llvm::SaveAndRestore<const clang::TemplateSpecializationType*> plain(substitution, nullptr);
        return qualified(same(argument->getAsType()));
    }
    /// A builtin transform, `__remove_cv(int)`, is the library's spelling.
    if(const auto* transform = llvm::dyn_cast<clang::UnaryTransformType>(T);
       transform && !transform->isDependentType()) {
        return beneath(transform->getUnderlyingType());
    }
    if(const auto* alias = llvm::dyn_cast<clang::TypedefType>(T)) {
        const auto* decl = alias->getDecl();
        if(internal(decl)) {
            return beneath(alias->desugar());
        }
        if(alias->isDependentType()) {
            return type;
        }
        if(sugar != Sugar::Written && computes(*decl)) {
            return beneath(alias->desugar());
        }
        auto named = qualifier(*decl, scope, source);
        auto shown =
            !renamed(named)
                ? type
                : qualified(
                      context.getTypedefType(clang::ElaboratedTypeKeyword::None, named, decl));
        /// Another member alias beneath names the type no better, only
        /// otherwise (`vector<bool>::pointer` for its `iterator`).
        if(sugar == Sugar::Library && specialization_member(*decl)) {
            auto shorter = beneath(alias->desugar());
            const auto* sibling = llvm::dyn_cast<clang::TypedefType>(shorter.getTypePtr());
            if(!shows_internal(shorter) &&
               !(sibling && specialization_member(*sibling->getDecl())) &&
               shorter.getAsString(policy).size() <= shown.getAsString(policy).size()) {
                return shorter;
            }
        }
        return shown;
    }
    if(const auto* pointer = llvm::dyn_cast<clang::PointerType>(T)) {
        return qualified(context.getPointerType(same(pointer->getPointeeType())));
    }
    if(const auto* reference = llvm::dyn_cast<clang::LValueReferenceType>(T)) {
        return qualified(context.getLValueReferenceType(same(reference->getPointeeType())));
    }
    if(const auto* reference = llvm::dyn_cast<clang::RValueReferenceType>(T)) {
        return qualified(context.getRValueReferenceType(same(reference->getPointeeType())));
    }
    if(const auto* member = llvm::dyn_cast<clang::MemberPointerType>(T)) {
        return qualified(context.getMemberPointerType(same(member->getPointeeType()),
                                                      member->getQualifier(),
                                                      member->getMostRecentCXXRecordDecl()));
    }
    if(const auto* paren = llvm::dyn_cast<clang::ParenType>(T)) {
        return qualified(context.getParenType(same(paren->getInnerType())));
    }
    if(const auto* array = llvm::dyn_cast<clang::ConstantArrayType>(T)) {
        return qualified(context.getConstantArrayType(same(array->getElementType()),
                                                      array->getSize(),
                                                      array->getSizeExpr(),
                                                      array->getSizeModifier(),
                                                      array->getIndexTypeCVRQualifiers()));
    }
    if(const auto* array = llvm::dyn_cast<clang::IncompleteArrayType>(T)) {
        return qualified(context.getIncompleteArrayType(same(array->getElementType()),
                                                        array->getSizeModifier(),
                                                        array->getIndexTypeCVRQualifiers()));
    }
    if(const auto* function = llvm::dyn_cast<clang::FunctionProtoType>(T);
       function && !function->isDependentType()) {
        llvm::SmallVector<clang::QualType, 4> params;
        for(auto param: function->getParamTypes()) {
            params.push_back(same(param));
        }
        return qualified(context.getFunctionType(same(function->getReturnType()),
                                                 params,
                                                 function->getExtProtoInfo()));
    }

    auto keyword = clang::ElaboratedTypeKeyword::None;
    clang::TemplateName name;
    llvm::ArrayRef<clang::TemplateArgument> arguments;
    const clang::ClassTemplateSpecializationDecl* spec_scope = nullptr;
    if(const auto* TST = llvm::dyn_cast<clang::TemplateSpecializationType>(T)) {
        const auto* pattern = TST->getTemplateName().getAsTemplateDecl();
        if(const auto* alias = llvm::dyn_cast_if_present<clang::TypeAliasTemplateDecl>(pattern);
           alias && TST->isTypeAlias() &&
           (internal(alias) ||
            (sugar != Sugar::Written && !TST->isDependentType() && computes(*alias)))) {
            return beneath(TST->getAliasedType());
        }
        /// A builtin template, `__type_pack_element<1, int, double>`, is the
        /// compiler's computation.
        if(llvm::isa_and_present<clang::BuiltinTemplateDecl>(pattern) && TST->isSugared()) {
            return beneath(TST->desugar());
        }
        const auto* spec = llvm::dyn_cast_if_present<clang::ClassTemplateSpecializationDecl>(
            TST->getAsCXXRecordDecl());
        if(const auto* alias =
               aliasing && spec && !TST->isTypeAlias() ? standard_alias(*spec) : nullptr) {
            return qualified(context.getTypedefType(clang::ElaboratedTypeKeyword::None,
                                                    qualifier(*alias, scope, source),
                                                    alias));
        }
        keyword = TST->getKeyword();
        name = TST->getTemplateName();
        arguments = TST->template_arguments();
        /// Even unqualified, a template's name says it is written as is: a
        /// bare one prints its scope in template arguments.
        if(pattern && !llvm::isa<clang::TemplateTemplateParmDecl>(pattern)) {
            if(auto named = qualifier(*pattern, scope, source); renamed(named)) {
                name = context.getQualifiedTemplateName(
                    named,
                    /*TemplateKeyword=*/false,
                    clang::TemplateName(const_cast<clang::TemplateDecl*>(pattern)));
            }
        }
    } else if(const auto* tag = llvm::dyn_cast<clang::TagType>(T)) {
        const auto* decl = tag->getDecl();
        /// A class template's own class is its specialization by its own
        /// parameters.
        if(const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(decl);
           record && substitution && record->getDescribedClassTemplate()) {
            const auto* named = substitution->getTemplateName().getAsTemplateDecl();
            if(named && named->getCanonicalDecl() ==
                            record->getDescribedClassTemplate()->getCanonicalDecl()) {
                auto specialization = clang::QualType(substitution, 0);
                llvm::SaveAndRestore<const clang::TemplateSpecializationType*> plain(substitution,
                                                                                     nullptr);
                return qualified(same(specialization));
            }
        }
        if(tag->isDependentType()) {
            return type;
        }
        /// Naming the alias's class may read this class again: an argument
        /// of a function type, a class derived from the alias's.
        if(aliasing && internal(decl) && naming.insert(decl).second) {
            auto named = llvm::make_scope_exit([&] { naming.erase(decl); });
            if(const auto* alias = public_alias(*decl, source)) {
                return qualified(context.getTypedefType(clang::ElaboratedTypeKeyword::None,
                                                        qualifier(*alias, scope, source),
                                                        alias));
            }
        }
        const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl);
        if(!spec) {
            /// A canonical tag type prints the scope it is in, named from the
            /// scope by the printer; C writes it with its keyword.
            if(tag->isCanonicalUnqualified() && (scoped || !context.getLangOpts().CPlusPlus)) {
                return type;
            }
            auto named = qualifier(*decl, scope, source);
            if(!renamed(named)) {
                return type;
            }
            return qualified(context.getTagType(tag->getKeyword(), named, decl, false));
        }
        if(const auto* alias = aliasing ? standard_alias(*spec) : nullptr) {
            return qualified(context.getTypedefType(clang::ElaboratedTypeKeyword::None,
                                                    qualifier(*alias, scope, source),
                                                    alias));
        }
        /// A canonical tag type prints the scope it is in; the template-id
        /// it gives way to below takes the qualifier that names it.
        spec_scope = spec;
        name = clang::TemplateName(spec->getSpecializedTemplate());
        arguments = spec->getTemplateArgs().asArray();
    } else {
        return type;
    }

    llvm::SmallVector<clang::TemplateArgument, 4> rewritten;
    for(const auto& argument: arguments) {
        rewritten.push_back(readable(argument, scope, source, sugar));
    }
    /// A specialization prints every argument it has; the template-id it
    /// gives way to leaves the defaulted ones out, as code writes it.
    if(spec_scope) {
        const auto* params = spec_scope->getSpecializedTemplate()->getTemplateParameters();
        /// Instantiation marks the arguments a default gave: one that only
        /// it can compute (`SmallVector`'s inline size) compares to nothing.
        while(!rewritten.empty() && rewritten.size() <= params->size() &&
              (arguments[rewritten.size() - 1].getIsDefaulted() ||
               clang::isSubstitutedDefaultArgument(context,
                                                   arguments[rewritten.size() - 1],
                                                   params->getParam(rewritten.size() - 1),
                                                   arguments.take_front(rewritten.size() - 1),
                                                   params->getDepth()))) {
            rewritten.pop_back();
        }
        if(auto named = qualifier(*spec_scope, scope, source); renamed(named)) {
            name = context.getQualifiedTemplateName(named, /*TemplateKeyword=*/false, name);
        }
    }
    /// A dependent one names another type once its template's parameters
    /// read as arguments.
    bool dependent = type->isDependentType() && substitution;
    llvm::SmallVector<clang::TemplateArgument, 4> canonical;
    for(const auto& argument: dependent ? llvm::ArrayRef(rewritten) : arguments) {
        canonical.push_back(context.getCanonicalTemplateArgument(argument));
    }
    clang::QualType underlying = context.getCanonicalType(type);
    if(dependent) {
        /// An alias template's specialization says what it aliases, which
        /// only substituting into the alias computes: it stands for itself.
        const auto* alias = llvm::dyn_cast<clang::TemplateSpecializationType>(T);
        underlying = alias && alias->isTypeAlias() ? context.getCanonicalTemplateSpecializationType(
                                                         clang::ElaboratedTypeKeyword::None,
                                                         context.getCanonicalTemplateName(name),
                                                         canonical)
                                                   : clang::QualType();
    }
    return qualified(
        context.getTemplateSpecializationType(keyword, name, rewritten, canonical, underlying));
}

auto ReadableTypes::readable(const clang::TemplateArgument& argument,
                             const clang::DeclContext* scope,
                             const clang::Expr* source,
                             Sugar sugar) -> clang::TemplateArgument {
    if(argument.getKind() == clang::TemplateArgument::Type) {
        return clang::TemplateArgument(readable(argument.getAsType(), scope, source, sugar),
                                       /*isNullPtr=*/false,
                                       argument.getIsDefaulted());
    }
    /// An argument naming what only the template has, `sizeof...(Ts)`, reads
    /// as its value.
    if(argument.getKind() == clang::TemplateArgument::Expression) {
        const auto* expr = argument.getAsExpr();
        if(!expr->isValueDependent() && template_bound(*expr)) {
            if(auto value = expr->getIntegerConstantExpr(context)) {
                return clang::TemplateArgument(context,
                                               *value,
                                               expr->getType(),
                                               argument.getIsDefaulted());
            }
        }
    }
    if(argument.getKind() == clang::TemplateArgument::Pack) {
        llvm::SmallVector<clang::TemplateArgument, 4> elements;
        for(const auto& element: argument.pack_elements()) {
            elements.push_back(readable(element, scope, source, sugar));
        }
        return clang::TemplateArgument::CreatePackCopy(context, elements);
    }
    return argument;
}

}  // namespace clice::display
