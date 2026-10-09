module;

#include "modules/prelude.h"

/// Parts of this file are ported from clangd's AST.cpp, Hover.cpp,
/// InlayHints.cpp and CodeCompletionStrings.cpp (llvmorg-21.1.8), part of
/// the LLVM project, licensed under Apache License v2.0 with LLVM
/// Exceptions. See https://llvm.org/LICENSE.txt for license information.

module clice;

import :semantic.display;
import :semantic.types;
import :support.format;

namespace clice::display {

namespace {

/// Derive the printing policy for the policy-driven renderers: the
/// context's policy with the option-selected flags applied on top.
auto derive_policy(clang::ASTContext& context, const Options& options) -> clang::PrintingPolicy {
    clang::PrintingPolicy policy = context.getPrintingPolicy();
    policy.SuppressScope = options.suppress_scope;
    policy.SuppressUnwrittenScope = options.suppress_unwritten_scope;
    policy.TerseOutput = options.terse;
    policy.PolishForDeclaration = options.polish_for_declaration;
    policy.ConstantsAsWritten = options.constants_as_written;
    policy.SuppressTemplateArgsInCXXConstructors = options.suppress_ctor_template_args;
    policy.AnonymousTagNameStyle = std::to_underlying(
        options.anonymous_tag_locations ? clang::PrintingPolicy::AnonymousTagMode::SourceLocation
                                        : clang::PrintingPolicy::AnonymousTagMode::Plain);
    return policy;
}

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

namespace {

/// The qualified name of the decl, skipping unwritten scopes like inline
/// and anonymous namespaces.
std::string qualified_name(const clang::NamedDecl& decl) {
    std::string name;
    llvm::raw_string_ostream os(name);
    clang::PrintingPolicy policy(decl.getASTContext().getLangOpts());
    /// Note that inline namespaces are treated as transparent scopes. This
    /// reflects the way they're most commonly used for lookup.
    policy.SuppressUnwrittenScope = true;
    /// (unnamed struct), not (unnamed struct at /path/to/foo.cc:42:1).
    /// In the language server, context is usually available and paths are
    /// mostly noise.
    policy.AnonymousTagNameStyle =
        std::to_underlying(clang::PrintingPolicy::AnonymousTagMode::Plain);
    decl.printQualifiedName(os, policy);
    assert(!llvm::StringRef(name).starts_with("::"));
    return name;
}

bool looks_like_doc_comment(llvm::StringRef comment) {
    /// We don't report comments that only contain "special" chars.
    /// This avoids reporting various delimiters, like:
    ///   =================
    ///   -----------------
    ///   *****************
    return comment.find_first_not_of("/*-= \t\r\n") != llvm::StringRef::npos;
}

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
            return Visit(E->getCallee());
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

    return Namer{}.Visit(expr);
}

auto comment(const clang::NamedDecl* decl) -> std::string {
    assert(decl);
    if(llvm::isa<clang::NamespaceDecl>(decl)) {
        /// Namespaces often have too many redecls for any particular redecl comment
        /// to be useful. Moreover, we often confuse file headers or generated
        /// comments with namespace comments. Therefore we choose to just ignore
        /// the comments for namespaces.
        return "";
    }

    const clang::ASTContext& context = decl->getASTContext();
    const clang::RawComment* comment = clang::getCompletionComment(context, decl);
    if(!comment) {
        return "";
    }

    std::string doc =
        comment->getFormattedText(context.getSourceManager(), context.getDiagnostics());
    if(!looks_like_doc_comment(doc)) {
        return "";
    }

    /// Clang requires source to be UTF-8, but doesn't enforce this in comments.
    if(!llvm::json::isUTF8(doc)) {
        doc = llvm::json::fixUTF8(doc);
    }
    return doc;
}

auto local_scope(const clang::Decl* decl) -> std::string {
    assert(decl);
    std::vector<std::string> scopes;

    auto name_of_type_decl = [](const clang::TypeDecl* decl) {
        if(!decl->getDeclName().isEmpty()) {
            clang::PrintingPolicy policy = decl->getASTContext().getPrintingPolicy();
            policy.SuppressScope = true;
            return types::declared_type(decl).getAsString(policy);
        }

        if(auto* record = llvm::dyn_cast<clang::RecordDecl>(decl)) {
            return ("(anonymous " + record->getKindName() + ")").str();
        }

        return std::string();
    };

    const clang::DeclContext* context = decl->getDeclContext();
    while(context) {
        if(const auto* type_decl = llvm::dyn_cast<clang::TypeDecl>(context)) {
            scopes.push_back(name_of_type_decl(type_decl));
        } else if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(context)) {
            scopes.push_back(function->getNameAsString());
        }
        context = context->getParent();
    }

    return llvm::join(llvm::reverse(scopes), "::");
}

auto namespace_scope(const clang::Decl* decl) -> std::string {
    assert(decl);
    const clang::DeclContext* context = decl->getDeclContext();

    if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(context)) {
        return namespace_scope(tag);
    }

    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(context)) {
        return namespace_scope(function);
    }

    if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
        /// Skip inline/anon namespaces.
        if(ns->isInline() || ns->isAnonymousNamespace()) {
            return namespace_scope(ns);
        }
    }

    if(const auto* named = llvm::dyn_cast<clang::NamedDecl>(context)) {
        return qualified_name(*named);
    }

    return "";
}

auto restored_scope(clang::QualType type, const Options& options) -> clang::NestedNameSpecifier {
    /// Complex cases (pointers/references, cv-qualifiers) are not
    /// attempted, mirroring the tag-keyword special case of type().
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

auto type(clang::ASTContext& context, clang::QualType type, const Options& options) -> Type {
    clang::PrintingPolicy policy = derive_policy(context, options);

    if(options.resolve_decltype) {
        /// TypePrinter doesn't resolve decltypes, so resolve them here.
        /// FIXME: This doesn't handle composite types that contain a decltype in
        /// them. We should rather have a printing policy for that.
        while(!type.isNull() && type->isDecltypeType()) {
            type = type->castAs<clang::DecltypeType>()->getUnderlyingType();
        }
    }

    Type result;
    llvm::raw_string_ostream os(result.text);

    /// Special case: if the outer type is a canonical tag type, then include
    /// the tag for extra clarity. This isn't very idiomatic, so don't attempt
    /// it for complex cases, including pointers/references, template
    /// specializations, etc.
    if(options.tag_keyword_prefix && !type.isNull() && !type.hasQualifiers() &&
       policy.SuppressTagKeyword) {
        if(auto* tag = llvm::dyn_cast<clang::TagType>(type.getTypePtr());
           tag && tag->isCanonicalUnqualified()) {
            os << tag->getDecl()->getKindName() << " ";
        }
    }

    if(auto scope = restored_scope(type, options)) {
        scope.print(os, policy);
    }
    type.print(os, policy);

    if(!type.isNull() && options.show_aka) {
        if(auto desugared = desugar(context, type)) {
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

}  // namespace

auto param(const clang::ParmVarDecl* param, const Options& options) -> Param {
    assert(param);
    Param result;
    result.type = type(param->getASTContext(), param->getType(), options);
    if(!param->getName().empty()) {
        result.name = param->getNameAsString();
    }
    if(const clang::Expr* arg = default_arg(param)) {
        result.default_value.emplace();
        llvm::raw_string_ostream os(*result.default_value);
        arg->printPretty(os, nullptr, derive_policy(param->getASTContext(), options));
    }
    return result;
}

auto template_params(const clang::TemplateParameterList* params, const Options& options)
    -> std::vector<Param> {
    assert(params);
    std::vector<Param> result;

    for(const clang::Decl* decl: *params) {
        Param param;
        if(const auto* type_param = llvm::dyn_cast<clang::TemplateTypeParmDecl>(decl)) {
            param.type = param_type(*type_param);

            if(!type_param->getName().empty()) {
                param.name = type_param->getNameAsString();
            }

            if(type_param->hasDefaultArgument()) {
                param.default_value.emplace();
                llvm::raw_string_ostream out(*param.default_value);
                type_param->getDefaultArgument().getArgument().print(
                    derive_policy(type_param->getASTContext(), options),
                    out,
                    /*IncludeType=*/false);
            }
        } else if(const auto* non_type_param =
                      llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(decl)) {
            param.type = param_type(*non_type_param, options);

            if(auto* identifier = non_type_param->getIdentifier()) {
                param.name = identifier->getName().str();
            }

            if(non_type_param->hasDefaultArgument()) {
                param.default_value.emplace();
                llvm::raw_string_ostream out(*param.default_value);
                non_type_param->getDefaultArgument().getArgument().print(
                    derive_policy(non_type_param->getASTContext(), options),
                    out,
                    /*IncludeType=*/false);
            }
        } else if(const auto* template_param =
                      llvm::dyn_cast<clang::TemplateTemplateParmDecl>(decl)) {
            param.type = param_type(*template_param, options);

            if(!template_param->getName().empty()) {
                param.name = template_param->getNameAsString();
            }

            if(template_param->hasDefaultArgument()) {
                param.default_value.emplace();
                llvm::raw_string_ostream out(*param.default_value);
                template_param->getDefaultArgument().getArgument().print(
                    derive_policy(template_param->getASTContext(), options),
                    out,
                    /*IncludeType=*/false);
            }
        }
        result.push_back(std::move(param));
    }

    return result;
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
/// anyway.
bool is_huge(const clang::Expr* init, const clang::PrintingPolicy& policy, TokenCount token_count) {
    return init &&
           (token_count(init->getSourceRange()) > 200 || printed_length(*init, policy) > 500);
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
               init && !is_huge(init, policy, token_count)) {
                os << " = ";
                init->printPretty(os, nullptr, policy);
            } else if(!enumerator->getType()->isDependentType()) {
                os << " = " << llvm::toString(enumerator->getInitVal(), 10);
            }
        }
    }

    void members(const clang::RecordDecl& record) {
        auto access = record.isClass() ? clang::AS_private : clang::AS_public;
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
            } else {
                print(*member, policy);
            }
            os << ';';
        }
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
        if(is_huge(initializer(member), member_policy, token_count)) {
            member_policy.SuppressInitializers = true;
        }
        member.print(os, member_policy);
    }
};

}  // namespace

auto definition(const clang::Decl* decl,
                const Options& options,
                llvm::function_ref<std::size_t(clang::SourceRange)> token_count) -> std::string {
    assert(decl);
    clang::PrintingPolicy policy = derive_policy(decl->getASTContext(), options);
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl);
       var && is_huge(var->getInit(), policy, token_count)) {
        policy.SuppressInitializers = true;
    }

    std::string definition;
    llvm::raw_string_ostream os(definition);
    const auto* class_template = llvm::dyn_cast<clang::ClassTemplateDecl>(decl);
    const auto* tag =
        llvm::dyn_cast<clang::TagDecl>(class_template ? class_template->getTemplatedDecl() : decl);
    if(!tag || !tag->getDefinition()) {
        decl->print(os, policy);
        return definition;
    }

    /// The head comes from the definition too: only a definition prints its
    /// bases. tag_head() relies on the terse body.
    assert(options.terse);
    tag = tag->getDefinition();
    if(class_template) {
        os << tag_head(*llvm::cast<clang::CXXRecordDecl>(tag)->getDescribedClassTemplate(), policy);
    } else {
        os << tag_head(*tag, policy);
    }
    if(options.max_members > 0) {
        MemberPrinter printer{
            .os = os,
            .policy = policy,
            .token_count = token_count,
            .budget = options.max_members,
        };
        printer.body(*tag);
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
    if(param.type) {
        os << param.type->text;
    }
    if(param.name) {
        os << " " << *param.name;
    }
    if(param.default_value) {
        os << " = " << *param.default_value;
    }
    if(param.type && param.type->aka) {
        os << " (aka " << *param.type->aka << ")";
    }
    return os;
}

}  // namespace clice::display
