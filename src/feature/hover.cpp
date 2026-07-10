/// Ported from clangd's Hover.cpp (llvmorg-21.1.8), part of the LLVM
/// project, licensed under Apache License v2.0 with LLVM Exceptions.
/// See https://llvm.org/LICENSE.txt for license information.

#include <optional>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "semantic/ast_utility.h"
#include "semantic/find_target.h"
#include "semantic/selection.h"
#include "syntax/lexer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Format/Format.h"
#include "clang/Sema/HeuristicResolver.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Syntax/Tokens.h"

namespace clice::feature {

namespace {

using PrintedType = HoverInfo::PrintedType;
using Param = HoverInfo::Param;
using PassType = HoverInfo::PassType;
using PassMode = HoverInfo::PassMode;

auto hover_printing_policy(clang::PrintingPolicy base) -> clang::PrintingPolicy {
    base.AnonymousTagLocations = false;
    base.TerseOutput = true;
    base.PolishForDeclaration = true;
    base.ConstantsAsWritten = true;
    base.SuppressTemplateArgsInCXXConstructors = true;
    return base;
}

/// Given a declaration, return a human-readable string representing the
/// local scope in which it is declared, i.e. class(es) and method name.
/// Returns an empty string if it is not local.
auto local_scope(const clang::Decl* decl) -> std::string {
    std::vector<std::string> scopes;

    auto name_of_type_decl = [](const clang::TypeDecl* decl) {
        if(!decl->getDeclName().isEmpty()) {
            clang::PrintingPolicy policy = decl->getASTContext().getPrintingPolicy();
            policy.SuppressScope = true;
            return ast::declared_type(decl).getAsString(policy);
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

/// Returns the human-readable representation for namespace containing the
/// declaration. Returns empty if it is contained in the global namespace.
auto namespace_scope(const clang::Decl* decl) -> std::string {
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
        return ast::print_qualified_name(*named);
    }

    return "";
}

auto print_definition(const clang::Decl* decl,
                      clang::PrintingPolicy policy,
                      const clang::syntax::TokenBuffer& tb) -> std::string {
    if(auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        if(auto* init = var->getInit()) {
            /// Initializers might be huge and result in lots of memory allocations
            /// in some catastrophic cases. Such long lists are not useful in hover
            /// cards anyway.
            if(tb.expandedTokens(init->getSourceRange()).size() > 200) {
                policy.SuppressInitializers = true;
            }
        }
    }

    std::string definition;
    llvm::raw_string_ostream os(definition);
    decl->print(os, policy);
    return definition;
}

auto print_type(clang::QualType type,
                clang::ASTContext& context,
                const clang::PrintingPolicy& policy,
                const HoverOptions& options) -> PrintedType {
    /// TypePrinter doesn't resolve decltypes, so resolve them here.
    /// FIXME: This doesn't handle composite types that contain a decltype in
    /// them. We should rather have a printing policy for that.
    while(!type.isNull() && type->isDecltypeType()) {
        type = type->castAs<clang::DecltypeType>()->getUnderlyingType();
    }

    PrintedType result;
    llvm::raw_string_ostream os(result.type);

    /// Special case: if the outer type is a tag type without qualifiers, then
    /// include the tag for extra clarity. This isn't very idiomatic, so don't
    /// attempt it for complex cases, including pointers/references, template
    /// specializations, etc.
    if(!type.isNull() && !type.hasQualifiers() && policy.SuppressTagKeyword) {
        if(auto* tag = llvm::dyn_cast<clang::TagType>(type.getTypePtr())) {
            os << tag->getDecl()->getKindName() << " ";
        }
    }
    type.print(os, policy);

    if(!type.isNull() && options.show_aka) {
        bool should_aka = false;
        clang::QualType desugared = clang::desugarForDiagnostic(context, type, should_aka);
        if(should_aka) {
            result.aka = desugared.getAsString(policy);
        }
    }
    return result;
}

auto print_type(const clang::TemplateTypeParmDecl* param) -> PrintedType {
    PrintedType result;
    result.type = param->wasDeclaredWithTypename() ? "typename" : "class";
    if(param->isParameterPack()) {
        result.type += "...";
    }
    return result;
}

auto print_type(const clang::NonTypeTemplateParmDecl* param,
                const clang::PrintingPolicy& policy,
                const HoverOptions& options) -> PrintedType {
    auto result = print_type(param->getType(), param->getASTContext(), policy, options);
    if(param->isParameterPack()) {
        result.type += "...";
        if(result.aka) {
            *result.aka += "...";
        }
    }
    return result;
}

auto print_type(const clang::TemplateTemplateParmDecl* param,
                const clang::PrintingPolicy& policy,
                const HoverOptions& options) -> PrintedType {
    PrintedType result;
    llvm::raw_string_ostream os(result.type);
    os << "template <";
    llvm::StringRef sep = "";
    for(const clang::Decl* decl: *param->getTemplateParameters()) {
        os << sep;
        sep = ", ";
        if(const auto* type_param = llvm::dyn_cast<clang::TemplateTypeParmDecl>(decl)) {
            os << print_type(type_param).type;
        } else if(const auto* non_type_param =
                      llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(decl)) {
            os << print_type(non_type_param, policy, options).type;
        } else if(const auto* template_param =
                      llvm::dyn_cast<clang::TemplateTemplateParmDecl>(decl)) {
            os << print_type(template_param, policy, options).type;
        }
    }
    /// FIXME: TemplateTemplateParameter doesn't store the info on whether this
    /// param was a "typename" or "class".
    os << "> class";
    return result;
}

auto fetch_template_parameters(const clang::TemplateParameterList* params,
                               const clang::PrintingPolicy& policy,
                               const HoverOptions& options) -> std::vector<Param> {
    assert(params);
    std::vector<Param> result;

    for(const clang::Decl* decl: *params) {
        Param param;
        if(const auto* type_param = llvm::dyn_cast<clang::TemplateTypeParmDecl>(decl)) {
            param.type = print_type(type_param);

            if(!type_param->getName().empty()) {
                param.name = type_param->getNameAsString();
            }

            if(type_param->hasDefaultArgument()) {
                param.default_value.emplace();
                llvm::raw_string_ostream out(*param.default_value);
                type_param->getDefaultArgument().getArgument().print(policy,
                                                                     out,
                                                                     /*IncludeType=*/false);
            }
        } else if(const auto* non_type_param =
                      llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(decl)) {
            param.type = print_type(non_type_param, policy, options);

            if(auto* identifier = non_type_param->getIdentifier()) {
                param.name = identifier->getName().str();
            }

            if(non_type_param->hasDefaultArgument()) {
                param.default_value.emplace();
                llvm::raw_string_ostream out(*param.default_value);
                non_type_param->getDefaultArgument().getArgument().print(policy,
                                                                         out,
                                                                         /*IncludeType=*/false);
            }
        } else if(const auto* template_param =
                      llvm::dyn_cast<clang::TemplateTemplateParmDecl>(decl)) {
            param.type = print_type(template_param, policy, options);

            if(!template_param->getName().empty()) {
                param.name = template_param->getNameAsString();
            }

            if(template_param->hasDefaultArgument()) {
                param.default_value.emplace();
                llvm::raw_string_ostream out(*param.default_value);
                template_param->getDefaultArgument().getArgument().print(policy,
                                                                         out,
                                                                         /*IncludeType=*/false);
            }
        }
        result.push_back(std::move(param));
    }

    return result;
}

auto underlying_function(const clang::Decl* decl) -> const clang::FunctionDecl* {
    /// Extract lambda from variables.
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        auto type = var->getType();
        if(!type.isNull()) {
            while(!type->getPointeeType().isNull()) {
                type = type->getPointeeType();
            }

            if(const auto* record = type->getAsCXXRecordDecl()) {
                return record->getLambdaCallOperator();
            }
        }
    }

    /// Non-lambda functions.
    return decl->getAsFunction();
}

/// Returns the decl that should be used for querying comments.
auto decl_for_comment(const clang::NamedDecl* decl) -> const clang::NamedDecl* {
    const clang::NamedDecl* result = decl;
    if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
        /// Template may not be instantiated e.g. if the type didn't need to be
        /// complete; fallback to primary template.
        if(spec->getTemplateSpecializationKind() == clang::TSK_Undeclared) {
            result = spec->getSpecializedTemplate();
        } else if(const auto* pattern = spec->getTemplateInstantiationPattern()) {
            result = pattern;
        }
    } else if(const auto* spec = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(decl)) {
        if(spec->getTemplateSpecializationKind() == clang::TSK_Undeclared) {
            result = spec->getSpecializedTemplate();
        } else if(const auto* pattern = spec->getTemplateInstantiationPattern()) {
            result = pattern;
        }
    } else if(const auto* function = decl->getAsFunction()) {
        if(const auto* pattern = function->getTemplateInstantiationPattern()) {
            result = pattern;
        }
    }

    /// Ensure that decl_for_comment(decl_for_comment(X)) = decl_for_comment(X).
    /// This is usually not needed, but in strange cases of comparison operators
    /// being instantiated from spaceship operator, which itself is a template
    /// instantiation the recursive call is necessary.
    if(decl != result) {
        result = decl_for_comment(result);
    }
    return result;
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

auto to_hover_param(const clang::ParmVarDecl* param,
                    const clang::PrintingPolicy& policy,
                    const HoverOptions& options) -> Param {
    Param result;
    result.type = print_type(param->getType(), param->getASTContext(), policy, options);
    if(!param->getName().empty()) {
        result.name = param->getNameAsString();
    }
    if(const clang::Expr* arg = default_arg(param)) {
        result.default_value.emplace();
        llvm::raw_string_ostream os(*result.default_value);
        arg->printPretty(os, nullptr, policy);
    }
    return result;
}

/// Populates type, return_type, and parameters for function-like decls.
void fill_function_type_and_params(HoverInfo& info,
                                   const clang::Decl* decl,
                                   const clang::FunctionDecl* function,
                                   const clang::PrintingPolicy& policy,
                                   const HoverOptions& options) {
    info.parameters.emplace();
    for(const clang::ParmVarDecl* param: function->parameters()) {
        info.parameters->emplace_back(to_hover_param(param, policy, options));
    }

    /// We don't want any type info, if name already contains it. This is true
    /// for constructors/destructors and conversion operators.
    const auto name_kind = function->getDeclName().getNameKind();
    if(name_kind == clang::DeclarationName::CXXConstructorName ||
       name_kind == clang::DeclarationName::CXXDestructorName ||
       name_kind == clang::DeclarationName::CXXConversionFunctionName) {
        return;
    }

    auto& context = function->getASTContext();
    info.return_type = print_type(function->getReturnType(), context, policy, options);
    clang::QualType type = function->getType();
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        /// Lambdas.
        type = var->getType().getDesugaredType(decl->getASTContext());
    }
    info.type = print_type(type, decl->getASTContext(), policy, options);
    /// FIXME: handle variadics.
}

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

auto print_expr_value(const clang::Expr* expr, const clang::ASTContext& context)
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
    if(type->isEnumeralType() && constant.Val.isInt() &&
       constant.Val.getInt().getSignificantBits() <= 64) {
        /// Compare to int64_t to avoid bit-width match requirements.
        std::int64_t value = constant.Val.getInt().getExtValue();
        for(const clang::EnumConstantDecl* enumerator:
            type->castAs<clang::EnumType>()->getDecl()->enumerators()) {
            if(enumerator->getInitVal() == value) {
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

    return constant.Val.getAsString(context, type);
}

struct PrintExprResult {
    /// The evaluation result on the expression.
    std::optional<std::string> printed_value;

    /// The expr object that represents the closest evaluable expression.
    const clang::Expr* expr;

    /// The node of the selection tree where the traversal stops.
    const SelectionTree::Node* node;
};

/// Seek the closest evaluable expression along the ancestors of node N in a
/// selection tree. If a node in the path can be converted to an evaluable
/// Expr, a possible evaluation would happen and the associated context is
/// returned. If evaluation couldn't be done, return the node where the
/// traversal ends.
auto print_expr_value(const SelectionTree::Node* node, const clang::ASTContext& context)
    -> PrintExprResult {
    for(; node; node = node->parent) {
        /// Try to evaluate the first evaluatable enclosing expression.
        if(const auto* expr = node->get<clang::Expr>()) {
            /// Once we cross an expression of type 'cv void', the evaluated
            /// result has nothing to do with our original cursor position.
            if(!expr->getType().isNull() && expr->getType()->isVoidType()) {
                break;
            }

            if(auto value = print_expr_value(expr, context)) {
                return PrintExprResult{
                    .printed_value = std::move(value),
                    .expr = expr,
                    .node = node,
                };
            }
        } else if(node->get<clang::Decl>() || node->get<clang::Stmt>()) {
            /// Refuse to cross certain non-exprs. (TypeLoc are OK as part of
            /// Exprs). This tries to ensure we're showing a value related to
            /// the cursor.
            break;
        }
    }

    return PrintExprResult{
        .printed_value = std::nullopt,
        .expr = nullptr,
        .node = node,
    };
}

auto field_name(const clang::Expr* expr) -> std::optional<llvm::StringRef> {
    const auto* member = llvm::dyn_cast<clang::MemberExpr>(expr->IgnoreCasts());
    if(!member || !llvm::isa<clang::CXXThisExpr>(member->getBase()->IgnoreCasts())) {
        return std::nullopt;
    }

    const auto* field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    if(!field || !field->getDeclName().isIdentifier()) {
        return std::nullopt;
    }

    return field->getDeclName().getAsIdentifierInfo()->getName();
}

/// If the method is of the form `T foo() { return FieldName; }` then returns
/// "FieldName".
auto getter_variable_name(const clang::CXXMethodDecl* method) -> std::optional<llvm::StringRef> {
    assert(method->hasBody());
    if(method->getNumParams() != 0 || method->isVariadic()) {
        return std::nullopt;
    }

    const auto* body = llvm::dyn_cast<clang::CompoundStmt>(method->getBody());
    const auto* only_return = (body && body->size() == 1)
                                  ? llvm::dyn_cast<clang::ReturnStmt>(body->body_front())
                                  : nullptr;
    if(!only_return || !only_return->getRetValue()) {
        return std::nullopt;
    }

    return field_name(only_return->getRetValue());
}

/// If the method is one of the forms:
///   void foo(T arg) { FieldName = arg; }
///   R foo(T arg) { FieldName = arg; return *this; }
///   void foo(T arg) { FieldName = std::move(arg); }
///   R foo(T arg) { FieldName = std::move(arg); return *this; }
/// then returns "FieldName".
auto setter_variable_name(const clang::CXXMethodDecl* method) -> std::optional<llvm::StringRef> {
    assert(method->hasBody());
    if(method->isConst() || method->getNumParams() != 1 || method->isVariadic()) {
        return std::nullopt;
    }

    const clang::ParmVarDecl* arg = method->getParamDecl(0);
    if(arg->isParameterPack()) {
        return std::nullopt;
    }

    const auto* body = llvm::dyn_cast<clang::CompoundStmt>(method->getBody());
    if(!body || body->size() == 0 || body->size() > 2) {
        return std::nullopt;
    }

    /// If the second statement exists, it must be `return this` or
    /// `return *this`.
    if(body->size() == 2) {
        auto* ret = llvm::dyn_cast<clang::ReturnStmt>(body->body_back());
        if(!ret || !ret->getRetValue()) {
            return std::nullopt;
        }

        const clang::Expr* ret_value = ret->getRetValue()->IgnoreCasts();
        if(const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(ret_value)) {
            if(unary->getOpcode() != clang::UO_Deref) {
                return std::nullopt;
            }
            ret_value = unary->getSubExpr()->IgnoreCasts();
        }

        if(!llvm::isa<clang::CXXThisExpr>(ret_value)) {
            return std::nullopt;
        }
    }

    /// The first statement must be an assignment of the arg to a field.
    const clang::Expr* lhs;
    const clang::Expr* rhs;
    if(const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(body->body_front())) {
        if(binary->getOpcode() != clang::BO_Assign) {
            return std::nullopt;
        }
        lhs = binary->getLHS();
        rhs = binary->getRHS();
    } else if(const auto* operator_call =
                  llvm::dyn_cast<clang::CXXOperatorCallExpr>(body->body_front())) {
        if(operator_call->getOperator() != clang::OO_Equal || operator_call->getNumArgs() != 2) {
            return std::nullopt;
        }
        lhs = operator_call->getArg(0);
        rhs = operator_call->getArg(1);
    } else {
        return std::nullopt;
    }

    /// Detect the case when the item is moved into the field.
    if(auto* call = llvm::dyn_cast<clang::CallExpr>(rhs->IgnoreCasts())) {
        if(call->getNumArgs() != 1) {
            return std::nullopt;
        }

        auto* callee = llvm::dyn_cast_or_null<clang::NamedDecl>(call->getCalleeDecl());
        if(!callee || !callee->getIdentifier() || callee->getName() != "move" ||
           !callee->isInStdNamespace()) {
            return std::nullopt;
        }
        rhs = call->getArg(0);
    }

    auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(rhs->IgnoreCasts());
    if(!ref || ref->getDecl() != arg) {
        return std::nullopt;
    }
    return field_name(lhs);
}

auto synthesize_documentation(const clang::NamedDecl* decl) -> std::string {
    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl)) {
        /// Is this an ordinary, non-static method whose definition is visible?
        if(method->getDeclName().isIdentifier() && !method->isStatic() &&
           (method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(method->getDefinition())) &&
           method->hasBody()) {
            if(const auto getter_field = getter_variable_name(method)) {
                return llvm::formatv("Trivial accessor for `{0}`.", *getter_field);
            }
            if(const auto setter_field = setter_variable_name(method)) {
                return llvm::formatv("Trivial setter for `{0}`.", *setter_field);
            }
        }
    }
    return "";
}

/// Generate a hover info given the declaration.
auto decl_hover(const clang::NamedDecl* decl,
                const clang::PrintingPolicy& policy,
                const clang::syntax::TokenBuffer& tb,
                const HoverOptions& options) -> HoverInfo {
    HoverInfo info;
    auto& context = decl->getASTContext();

    info.access_specifier = clang::getAccessSpelling(decl->getAccess()).str();
    info.namespace_scope = namespace_scope(decl);
    if(!info.namespace_scope->empty()) {
        info.namespace_scope->append("::");
    }
    info.local_scope = local_scope(decl);
    if(!info.local_scope.empty()) {
        info.local_scope.append("::");
    }

    info.name = ast::print_name(*decl);
    const auto* comment_decl = decl_for_comment(decl);
    info.documentation = ast::decl_comment(context, *comment_decl);
    if(info.documentation.empty()) {
        info.documentation = synthesize_documentation(decl);
    }

    info.kind = SymbolKind::from(decl);

    /// Fill in template params.
    if(const clang::TemplateDecl* template_decl = decl->getDescribedTemplate()) {
        info.template_parameters =
            fetch_template_parameters(template_decl->getTemplateParameters(), policy, options);
        decl = template_decl;
    } else if(const clang::FunctionDecl* function = decl->getAsFunction()) {
        if(const auto* function_template = function->getDescribedTemplate()) {
            info.template_parameters =
                fetch_template_parameters(function_template->getTemplateParameters(),
                                          policy,
                                          options);
            decl = function_template;
        }
    }

    /// Fill in types and params.
    if(const clang::FunctionDecl* function = underlying_function(decl)) {
        fill_function_type_and_params(info, decl, function, policy, options);
    } else if(const auto* value = llvm::dyn_cast<clang::ValueDecl>(decl)) {
        info.type = print_type(value->getType(), context, policy, options);
    } else if(const auto* type_param = llvm::dyn_cast<clang::TemplateTypeParmDecl>(decl)) {
        info.type = type_param->wasDeclaredWithTypename() ? "typename" : "class";
    } else if(const auto* template_param = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(decl)) {
        info.type = print_type(template_param, policy, options);
    } else if(const auto* var_template = llvm::dyn_cast<clang::VarTemplateDecl>(decl)) {
        info.type =
            print_type(var_template->getTemplatedDecl()->getType(), context, policy, options);
    } else if(const auto* typedef_decl = llvm::dyn_cast<clang::TypedefNameDecl>(decl)) {
        info.type = print_type(typedef_decl->getUnderlyingType().getDesugaredType(context),
                               context,
                               policy,
                               options);
    } else if(const auto* alias_template = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(decl)) {
        info.type = print_type(alias_template->getTemplatedDecl()->getUnderlyingType(),
                               context,
                               policy,
                               options);
    }

    /// Fill in value with evaluated initializer if possible.
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl); var && !var->isInvalidDecl()) {
        if(const clang::Expr* init = var->getInit()) {
            info.value = print_expr_value(init, context);
        }
    } else if(const auto* enumerator = llvm::dyn_cast<clang::EnumConstantDecl>(decl)) {
        /// Dependent enums (e.g. nested in template classes) don't have values yet.
        if(!enumerator->getType()->isDependentType()) {
            info.value = llvm::toString(enumerator->getInitVal(), 10);
        }
    }

    info.definition = print_definition(decl, policy, tb);
    return info;
}

/// The standard defines __func__ as a "predefined variable".
auto predefined_expr_hover(const clang::PredefinedExpr& expr,
                           clang::ASTContext& context,
                           const clang::PrintingPolicy& policy,
                           const HoverOptions& options) -> std::optional<HoverInfo> {
    HoverInfo info;
    info.name = expr.getIdentKindName();
    info.kind = SymbolKind::Variable;
    info.documentation = "Name of the current function (predefined variable)";
    if(const clang::StringLiteral* name = expr.getFunctionName()) {
        info.value.emplace();
        llvm::raw_string_ostream os(*info.value);
        name->outputString(os);
        info.type = print_type(name->getType(), context, policy, options);
    } else {
        /// Inside templates, the approximate type `const char[]` is still useful.
        clang::QualType string_type =
            context.getIncompleteArrayType(context.CharTy.withConst(),
                                           clang::ArraySizeModifier::Normal,
                                           /*IndexTypeQuals=*/0);
        info.type = print_type(string_type, context, policy, options);
    }
    return info;
}

auto type_as_definition(const PrintedType& type) -> std::string {
    std::string result;
    llvm::raw_string_ostream os(result);
    os << type.type;
    if(type.aka) {
        os << " // aka: " << *type.aka;
    }
    return result;
}

auto this_expr_hover(const clang::CXXThisExpr* expr,
                     clang::ASTContext& context,
                     const clang::PrintingPolicy& policy,
                     const HoverOptions& options) -> std::optional<HoverInfo> {
    clang::QualType origin_this_type = expr->getType()->getPointeeType();
    clang::QualType class_type = ast::declared_type(origin_this_type->getAsTagDecl());

    /// For partial specialization class, origin `this` pointee type will be
    /// parsed as `InjectedClassNameType`, which will output template arguments
    /// like "type-parameter-0-0". So we retrieve user written class type in
    /// this case.
    clang::QualType pretty_this_type = context.getPointerType(
        clang::QualType(class_type.getTypePtr(), origin_this_type.getCVRQualifiers()));

    HoverInfo info;
    info.name = "this";
    info.definition = type_as_definition(print_type(pretty_this_type, context, policy, options));
    return info;
}

/// Generate a hover info given the deduced type.
auto deduced_type_hover(clang::QualType type,
                        const clang::syntax::Token& token,
                        clang::ASTContext& context,
                        const clang::PrintingPolicy& policy,
                        const HoverOptions& options) -> HoverInfo {
    HoverInfo info;
    /// FIXME: distinguish decltype(auto) vs decltype(expr).
    info.name = clang::tok::getTokenName(token.kind());
    info.kind = SymbolKind::Type;

    if(type->isUndeducedAutoType()) {
        info.definition = "/* not deduced */";
    } else {
        info.definition = type_as_definition(print_type(type, context, policy, options));

        if(const auto* decl = type->getAsTagDecl()) {
            const auto* comment_decl = decl_for_comment(decl);
            info.documentation = ast::decl_comment(context, *comment_decl);
        }
    }

    return info;
}

auto string_literal_hover(const clang::StringLiteral* literal, const clang::PrintingPolicy& policy)
    -> HoverInfo {
    HoverInfo info;
    info.name = "string-literal";
    info.size = (literal->getLength() + 1) * literal->getCharByteWidth() * 8;
    info.type.emplace();
    info.type->type = literal->getType().getAsString(policy);
    return info;
}

bool is_literal(const clang::Expr* expr) {
    /// Unfortunately there's no common base Literal classes inherits from
    /// (apart from Expr), therefore these exclusions.
    return llvm::isa<clang::CompoundLiteralExpr,
                     clang::CXXBoolLiteralExpr,
                     clang::CXXNullPtrLiteralExpr,
                     clang::FixedPointLiteral,
                     clang::FloatingLiteral,
                     clang::ImaginaryLiteral,
                     clang::IntegerLiteral,
                     clang::StringLiteral,
                     clang::UserDefinedLiteral>(expr);
}

auto name_for_expr([[maybe_unused]] const clang::Expr* expr) -> llvm::StringLiteral {
    /// FIXME: Come up with names for `special` expressions.
    return llvm::StringLiteral("expression");
}

auto pass_mode(clang::QualType param_type) -> PassMode {
    if(param_type->isReferenceType()) {
        if(param_type->getPointeeType().isConstQualified()) {
            return PassMode::ConstRef;
        }
        return PassMode::Ref;
    }
    return PassMode::Value;
}

/// If the node is passed as an argument to a function, fill
/// info.callee_arg_info with information about that argument.
void maybe_add_callee_arg_info(const SelectionTree::Node* node,
                               HoverInfo& info,
                               const clang::PrintingPolicy& policy,
                               const HoverOptions& options) {
    const auto& outer = node->outer_implicit();
    if(!outer.parent) {
        return;
    }

    const clang::FunctionDecl* callee = nullptr;
    llvm::ArrayRef<const clang::Expr*> args;

    if(const auto* call = outer.parent->get<clang::CallExpr>()) {
        callee = call->getDirectCallee();
        args = {call->getArgs(), call->getNumArgs()};
    } else if(const auto* construct = outer.parent->get<clang::CXXConstructExpr>()) {
        callee = construct->getConstructor();
        args = {construct->getArgs(), construct->getNumArgs()};
    }

    if(!callee) {
        return;
    }

    /// For non-function-call-like operators (e.g. operator+, operator<<) it's
    /// not immediately obvious what the "passed as" would refer to and, given
    /// fixed function signature, the value would be very low anyway, so we
    /// choose to not support that. Both variadic functions and operator()
    /// (especially relevant for lambdas) should be supported in the future.
    if(callee->isOverloadedOperator() || callee->isVariadic()) {
        return;
    }

    PassType pass_type;

    auto parameters = ast::resolve_forwarding_params(callee);

    /// Find the argument index for the node.
    for(unsigned i = 0; i < args.size() && i < parameters.size(); ++i) {
        if(args[i] != outer.get<clang::Expr>()) {
            continue;
        }

        /// Extract matching argument from function declaration.
        if(const clang::ParmVarDecl* param = parameters[i]) {
            info.callee_arg_info.emplace(to_hover_param(param, policy, options));
            if(node == &outer) {
                pass_type.pass_by = pass_mode(param->getType());
            }
        }
        break;
    }

    if(!info.callee_arg_info) {
        return;
    }

    /// If we found a matching argument, also figure out if it's a
    /// [const-]reference. For this we need to walk up the AST from the arg
    /// itself to the CallExpr and check all implicit casts, constructor
    /// calls, etc.
    if(const auto* expr = node->get<clang::Expr>()) {
        if(expr->getType().isConstQualified()) {
            pass_type.pass_by = PassMode::ConstRef;
        }
    }

    for(auto* cast_node = node->parent; cast_node != outer.parent && !pass_type.converted;
        cast_node = cast_node->parent) {
        if(const auto* implicit_cast = cast_node->get<clang::ImplicitCastExpr>()) {
            switch(implicit_cast->getCastKind()) {
                case clang::CK_NoOp:
                case clang::CK_DerivedToBase:
                case clang::CK_UncheckedDerivedToBase: {
                    /// If it was a reference before, it's still a reference.
                    if(pass_type.pass_by != PassMode::Value) {
                        pass_type.pass_by = implicit_cast->getType().isConstQualified()
                                                ? PassMode::ConstRef
                                                : PassMode::Ref;
                    }
                    break;
                }

                case clang::CK_LValueToRValue:
                case clang::CK_ArrayToPointerDecay:
                case clang::CK_FunctionToPointerDecay:
                case clang::CK_NullToPointer:
                case clang::CK_NullToMemberPointer: {
                    /// No longer a reference, but we do not show this as type
                    /// conversion.
                    pass_type.pass_by = PassMode::Value;
                    break;
                }

                default: {
                    pass_type.pass_by = PassMode::Value;
                    pass_type.converted = true;
                    break;
                }
            }
        } else if(const auto* ctor_call = cast_node->get<clang::CXXConstructExpr>()) {
            /// We want to be smart about copy constructors. They should not
            /// show up as type conversion, but instead as passing by value.
            if(ctor_call->getConstructor()->isCopyConstructor()) {
                pass_type.pass_by = PassMode::Value;
            } else {
                pass_type.converted = true;
            }
        } else if(cast_node->get<clang::MaterializeTemporaryExpr>()) {
            /// Can't bind a non-const-ref to a temporary, so has to be
            /// const-ref.
            pass_type.pass_by = PassMode::ConstRef;
        } else {
            /// Unknown implicit node, assume type conversion.
            pass_type.pass_by = PassMode::Value;
            pass_type.converted = true;
        }
    }

    info.call_pass_type.emplace(pass_type);
}

/// Generates hover info for `this` and evaluatable expressions.
/// FIXME: Support hover for literals (esp user-defined).
auto expr_hover(const SelectionTree::Node* node,
                const clang::Expr* expr,
                clang::ASTContext& context,
                const clang::PrintingPolicy& policy,
                const HoverOptions& options) -> std::optional<HoverInfo> {
    std::optional<HoverInfo> info;

    if(const auto* literal = llvm::dyn_cast<clang::StringLiteral>(expr)) {
        /// Print the type and the size for string literals.
        info = string_literal_hover(literal, policy);
    } else if(is_literal(expr)) {
        /// There's not much value in hovering over "42" and getting a hover
        /// card saying "42 is an int", similar for most other literals.
        /// However, if we have callee_arg_info, it's still useful to show it.
        maybe_add_callee_arg_info(node, info.emplace(), policy, options);
        if(info->callee_arg_info) {
            /// FIXME: Might want to show the expression's value here instead?
            /// E.g. if the literal is in hex it might be useful to show the
            /// decimal value here.
            info->name = "literal";
            return info;
        }
        return std::nullopt;
    }

    /// For `this` expr we currently generate hover with pointee type.
    if(const auto* this_expr = llvm::dyn_cast<clang::CXXThisExpr>(expr)) {
        info = this_expr_hover(this_expr, context, policy, options);
    }

    if(const auto* predefined = llvm::dyn_cast<clang::PredefinedExpr>(expr)) {
        info = predefined_expr_hover(*predefined, context, policy, options);
    }

    /// For expressions we currently print the type and the value, iff it is
    /// evaluatable.
    if(auto value = print_expr_value(expr, context)) {
        info.emplace();
        info->type = print_type(expr->getType(), context, policy, options);
        info->value = *value;
        info->name = name_for_expr(expr).str();
    }

    if(info) {
        maybe_add_callee_arg_info(node, *info, policy, options);
    }

    return info;
}

/// Generates hover info for attributes.
auto attr_hover(const clang::Attr* attr, clang::ASTContext& context) -> std::optional<HoverInfo> {
    HoverInfo info;
    info.name = attr->getSpelling();
    if(attr->hasScope()) {
        info.local_scope = attr->getScopeName()->getName().str();
    }

    {
        llvm::raw_string_ostream os(info.definition);
        attr->printPretty(os, context.getPrintingPolicy());
    }

    info.documentation = clang::Attr::getDocumentation(attr->getKind()).str();
    return info;
}

auto include_hover(CompilationUnitRef unit, std::uint32_t offset) -> std::optional<HoverInfo> {
    auto interested = unit.interested_file();
    auto directives_it = unit.directives().find(interested);
    if(directives_it == unit.directives().end()) {
        return std::nullopt;
    }

    auto content = unit.interested_content();
    auto* lang_opts = &unit.lang_options();

    auto header_name = [&](LocalSourceRange range) -> std::string {
        auto arg = content.substr(range.begin, range.end - range.begin).trim();
        if(arg.size() >= 2 && ((arg.front() == '"' && arg.back() == '"') ||
                               (arg.front() == '<' && arg.back() == '>'))) {
            arg = arg.drop_front().drop_back();
        }
        return arg.str();
    };

    auto try_directive = [&](clang::SourceLocation loc,
                             clang::FileID target) -> std::optional<HoverInfo> {
        if(!target.isValid())
            return std::nullopt;
        auto [fid, directive_offset] = unit.decompose_location(loc);
        if(fid != interested || directive_offset >= content.size())
            return std::nullopt;
        auto range = find_directive_argument(content, directive_offset, lang_opts);
        if(!range || !range->contains(offset))
            return std::nullopt;

        HoverInfo info;
        info.name = header_name(*range);
        info.kind = SymbolKind::Header;
        info.definition = unit.file_path(target);
        info.symbol_range = *range;
        return info;
    };

    for(const auto& include: directives_it->second.includes) {
        if(auto info = try_directive(include.location, include.fid)) {
            return info;
        }
    }
    for(const auto& has_include: directives_it->second.has_includes) {
        if(auto info = try_directive(has_include.location, has_include.fid)) {
            return info;
        }
    }
    return std::nullopt;
}

void add_layout_info(const clang::NamedDecl& decl, HoverInfo& info) {
    if(decl.isInvalidDecl()) {
        return;
    }

    const auto& context = decl.getASTContext();
    if(auto* record = llvm::dyn_cast<clang::RecordDecl>(&decl)) {
        if(auto size = context.getTypeSizeInCharsIfKnown(record->getTypeForDecl())) {
            info.size = size->getQuantity() * 8;
        }
        if(!record->isDependentType() && record->isCompleteDefinition()) {
            info.align = context.getTypeAlign(record->getTypeForDecl());
        }
        return;
    }

    if(const auto* field = llvm::dyn_cast<clang::FieldDecl>(&decl)) {
        const auto* record = field->getParent();
        if(record) {
            record = record->getDefinition();
        }

        if(record && !record->isInvalidDecl() && !record->isDependentType()) {
            info.align = context.getTypeAlign(field->getType());
            const clang::ASTRecordLayout& layout = context.getASTRecordLayout(record);
            info.offset = layout.getFieldOffset(field->getFieldIndex());
            if(field->isBitField()) {
                info.size = field->getBitWidthValue();
            } else if(auto size = context.getTypeSizeInCharsIfKnown(field->getType())) {
                info.size = field->isZeroSize(context) ? 0 : size->getQuantity() * 8;
            }

            if(info.size) {
                std::uint64_t end_of_field = *info.offset + *info.size;

                /// Calculate padding following the field.
                if(!record->isUnion() && field->getFieldIndex() + 1 < layout.getFieldCount()) {
                    /// Measure padding up to the next class field.
                    std::uint64_t next_offset = layout.getFieldOffset(field->getFieldIndex() + 1);
                    if(next_offset >= end_of_field) {
                        /// Next field could be a bitfield!
                        info.padding = next_offset - end_of_field;
                    }
                } else {
                    /// Measure padding up to the end of the object.
                    info.padding = layout.getSize().getQuantity() * 8 - end_of_field;
                }
            }

            /// Offset in a union is always zero, so not really useful to report.
            if(record->isUnion()) {
                info.offset.reset();
            }
        }
        return;
    }
}

auto pick_decl_to_use(llvm::ArrayRef<const clang::NamedDecl*> candidates)
    -> const clang::NamedDecl* {
    if(candidates.empty()) {
        return nullptr;
    }

    /// This is e.g the case for
    ///     namespace ns { void foo(); }
    ///     void bar() { using ns::foo; f^oo(); }
    /// One declaration in candidates will refer to the using declaration,
    /// which isn't really useful for hover. So use the other one, which in
    /// this example would be the actual declaration of foo.
    if(candidates.size() <= 2) {
        if(llvm::isa<clang::UsingDecl>(candidates.front())) {
            return candidates.back();
        }
        return candidates.front();
    }

    /// For something like
    ///     namespace ns { void foo(int); void foo(char); }
    ///     using ns::foo;
    ///     template <typename T> void bar() { fo^o(T{}); }
    /// we actually want to show the using declaration, it's not clear which
    /// declaration to pick otherwise.
    auto base_decls = llvm::make_filter_range(candidates, [](const clang::NamedDecl* decl) {
        return llvm::isa<clang::UsingDecl>(decl);
    });
    if(std::distance(base_decls.begin(), base_decls.end()) == 1) {
        return *base_decls.begin();
    }

    return candidates.front();
}

/// Sizes (and padding) are shown in bytes if possible, otherwise in bits.
auto format_size(std::uint64_t size_in_bits) -> std::string {
    std::uint64_t value = size_in_bits % 8 == 0 ? size_in_bits / 8 : size_in_bits;
    const char* unit = value != 0 && value == size_in_bits ? "bit" : "byte";
    return llvm::formatv("{0} {1}{2}", value, unit, value == 1 ? "" : "s").str();
}

/// Offsets are shown in bytes + bits, so offsets of different fields can
/// always be easily compared.
auto format_offset(std::uint64_t offset_in_bits) -> std::string {
    const auto bytes = offset_in_bits / 8;
    const auto bits = offset_in_bits % 8;
    auto offset = format_size(bytes * 8);
    if(bits != 0) {
        offset += " and " + format_size(bits);
    }
    return offset;
}

auto symbol_kind_string(SymbolKind kind) -> llvm::StringRef {
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
        case SymbolKind::EnumMember: return "enumerator";
        case SymbolKind::Function: return "function";
        case SymbolKind::Method: return "method";
        case SymbolKind::Variable: return "variable";
        case SymbolKind::Parameter: return "parameter";
        case SymbolKind::Label: return "label";
        case SymbolKind::Macro: return "macro";
        default: return "";
    }
}

/// If the backtick at the offset starts a probable quoted range, return the
/// range (including the quotes).
auto backtick_quote_range(llvm::StringRef line, unsigned offset) -> std::optional<llvm::StringRef> {
    assert(line[offset] == '`');

    /// The open-quote is usually preceded by whitespace.
    llvm::StringRef prefix = line.substr(0, offset);
    constexpr llvm::StringLiteral before_start_chars = " \t(=";
    if(!prefix.empty() && !before_start_chars.contains(prefix.back())) {
        return std::nullopt;
    }

    /// The quoted string must be nonempty and usually has no leading/trailing
    /// whitespace.
    auto next = line.find('`', offset + 1);
    if(next == llvm::StringRef::npos) {
        return std::nullopt;
    }

    llvm::StringRef contents = line.slice(offset + 1, next);
    if(contents.empty() || clang::isWhitespace(contents.front()) ||
       clang::isWhitespace(contents.back())) {
        return std::nullopt;
    }

    /// The close-quote is usually followed by whitespace or punctuation.
    llvm::StringRef suffix = line.substr(next + 1);
    constexpr llvm::StringLiteral after_end_chars = " \t)=.,;:";
    if(!suffix.empty() && !after_end_chars.contains(suffix.front())) {
        return std::nullopt;
    }

    return line.slice(offset, next + 1);
}

void parse_documentation_line(llvm::StringRef line, markup::Paragraph& out) {
    /// Probably this is appendText(line), but scan for something interesting.
    for(unsigned i = 0; i < line.size(); ++i) {
        switch(line[i]) {
            case '`': {
                if(auto range = backtick_quote_range(line, i)) {
                    out.append_text(line.substr(0, i));
                    out.append_code(range->trim('`'), /*preserve=*/true);
                    return parse_documentation_line(line.substr(i + range->size()), out);
                }
                break;
            }
        }
    }
    out.append_text(line).append_space();
}

bool is_paragraph_break(llvm::StringRef rest) {
    return rest.ltrim(" \t").starts_with("\n");
}

bool punctuation_indicates_line_break(llvm::StringRef line) {
    constexpr llvm::StringLiteral punctuation = R"txt(.:,;!?)txt";

    line = line.rtrim();
    return !line.empty() && punctuation.contains(line.back());
}

bool is_hard_line_break_indicator(llvm::StringRef rest) {
    /// '-'/'*' md list, '@'/'\' documentation command, '>' md blockquote,
    /// '#' headings, '`' code blocks.
    constexpr llvm::StringLiteral line_break_indicators = R"txt(-*@\>#`)txt";

    rest = rest.ltrim(" \t");
    if(rest.empty()) {
        return false;
    }

    if(line_break_indicators.contains(rest.front())) {
        return true;
    }

    if(llvm::isDigit(rest.front())) {
        llvm::StringRef after_digit = rest.drop_while(llvm::isDigit);
        if(after_digit.starts_with(".") || after_digit.starts_with(")")) {
            return true;
        }
    }
    return false;
}

bool is_hard_line_break_after(llvm::StringRef line, llvm::StringRef rest) {
    /// Should we also consider whether the line is short?
    return punctuation_indicates_line_break(line) || is_hard_line_break_indicator(rest);
}

/// Reformat the definition code with clang-format to get a consistent
/// presentation. We currently always use the LLVM style, as the definition is
/// a single pretty-printed declaration rather than user written code.
void reformat_definition(HoverInfo& info) {
    if(info.definition.empty()) {
        return;
    }

    auto style = clang::format::getLLVMStyle();
    auto replacements = clang::format::reformat(style,
                                                info.definition,
                                                {clang::tooling::Range(0, info.definition.size())});

    auto formatted = clang::tooling::applyAllReplacements(info.definition, replacements);
    if(!formatted) {
        llvm::consumeError(formatted.takeError());
        return;
    }
    info.definition = *formatted;
}

auto to_protocol_hover(CompilationUnitRef unit,
                       const HoverInfo& info,
                       const HoverOptions& options,
                       PositionEncoding encoding) -> protocol::Hover {
    auto document = info.present();

    protocol::MarkupContent content;
    if(options.parse_comment_as_markdown) {
        content.kind = protocol::MarkupKind::markdown;
        content.value = document.as_markdown();
    } else {
        content.kind = protocol::MarkupKind::plain_text;
        content.value = document.as_plain_text();
    }

    protocol::Hover result{
        .contents = std::move(content),
    };

    if(info.symbol_range) {
        LineMap map(unit.interested_content(), unit.line_starts(), encoding);
        result.range = to_range(map, *info.symbol_range);
    }

    return result;
}

}  // namespace

void parse_documentation(llvm::StringRef input, markup::Document& output) {
    std::vector<llvm::StringRef> paragraph_lines;
    auto flush_paragraph = [&] {
        if(paragraph_lines.empty()) {
            return;
        }

        auto& paragraph = output.add_paragraph();
        for(llvm::StringRef line: paragraph_lines) {
            parse_documentation_line(line, paragraph);
        }
        paragraph_lines.clear();
    };

    llvm::StringRef line, rest;
    for(std::tie(line, rest) = input.split('\n'); !(line.empty() && rest.empty());
        std::tie(line, rest) = rest.split('\n')) {
        /// After a linebreak remove spaces to avoid 4 space markdown code
        /// blocks. FIXME: make flush_paragraph handle this.
        line = line.ltrim();
        if(!line.empty()) {
            paragraph_lines.push_back(line);
        }

        if(is_paragraph_break(rest) || is_hard_line_break_after(line, rest)) {
            flush_paragraph();
        }
    }
    flush_paragraph();
}

markup::Document HoverInfo::present() const {
    markup::Document output;

    /// Header contains a text of the form:
    /// variable `var`
    ///
    /// class `X`
    ///
    /// function `foo`
    ///
    /// expression
    ///
    /// Note that we are making use of a level-3 heading because VSCode renders
    /// level 1 and 2 headers in a huge font, see
    /// https://github.com/microsoft/vscode/issues/88417 for details.
    markup::Paragraph& header = output.add_heading(3);
    if(auto kind_string = symbol_kind_string(kind); !kind_string.empty()) {
        header.append_text(kind_string).append_space();
    }
    assert(!name.empty() && "hover triggered on a nameless symbol");
    header.append_code(name);

    /// Put a linebreak after header to increase readability.
    output.add_ruler();

    /// Print types on their own lines to reduce chances of getting line-wrapped
    /// by the editor, as they might be long.
    if(return_type) {
        /// For functions we display signature in a list form, e.g.:
        /// → `x`
        /// Parameters:
        /// - `bool param1`
        /// - `int param2 = 5`
        output.add_paragraph().append_text("→ ").append_code(llvm::to_string(*return_type));
    }

    if(parameters && !parameters->empty()) {
        output.add_paragraph().append_text("Parameters: ");
        markup::BulletList& list = output.add_bullet_list();
        for(const auto& param: *parameters) {
            list.add_item().add_paragraph().append_code(llvm::to_string(param));
        }
    }

    /// Don't print the type after parameters or return type as this will just
    /// duplicate the information.
    if(type && !return_type && !parameters) {
        output.add_paragraph().append_text("Type: ").append_code(llvm::to_string(*type));
    }

    if(value) {
        markup::Paragraph& paragraph = output.add_paragraph();
        paragraph.append_text("Value = ");
        paragraph.append_code(*value);
    }

    if(offset) {
        output.add_paragraph().append_text("Offset: " + format_offset(*offset));
    }

    if(size) {
        auto& paragraph = output.add_paragraph().append_text("Size: " + format_size(*size));
        if(padding && *padding != 0) {
            paragraph.append_text(llvm::formatv(" (+{0} padding)", format_size(*padding)).str());
        }
        if(align) {
            paragraph.append_text(", alignment " + format_size(*align));
        }
    }

    if(callee_arg_info) {
        assert(call_pass_type);
        std::string buffer;
        llvm::raw_string_ostream os(buffer);
        os << "Passed ";
        if(call_pass_type->pass_by != PassMode::Value) {
            os << "by ";
            if(call_pass_type->pass_by == PassMode::ConstRef) {
                os << "const ";
            }
            os << "reference ";
        }

        if(callee_arg_info->name) {
            os << "as " << *callee_arg_info->name;
        } else if(call_pass_type->pass_by == PassMode::Value) {
            os << "by value";
        }

        if(call_pass_type->converted && callee_arg_info->type) {
            os << " (converted to " << callee_arg_info->type->type << ")";
        }
        output.add_paragraph().append_text(buffer);
    }

    if(!documentation.empty()) {
        parse_documentation(documentation, output);
    }

    if(!definition.empty()) {
        output.add_ruler();
        std::string buffer;

        /// Append scope comment, dropping trailing "::". Note that we don't
        /// print anything for the global namespace, to not annoy non-c++
        /// projects or projects that are not making use of namespaces.
        if(!local_scope.empty()) {
            /// Container name, e.g. class, method, function. We might want to
            /// propagate some info about the container type to print function
            /// foo, class X, method X::bar, etc.
            buffer += "// In " + llvm::StringRef(local_scope).rtrim(':').str() + '\n';
        } else if(namespace_scope && !namespace_scope->empty()) {
            buffer +=
                "// In namespace " + llvm::StringRef(*namespace_scope).rtrim(':').str() + '\n';
        }

        if(!access_specifier.empty()) {
            buffer += access_specifier + ": ";
        }

        buffer += definition;

        output.add_code_block(std::move(buffer));
    }

    return output;
}

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const HoverInfo::PrintedType& type) {
    os << type.type;
    if(type.aka) {
        os << " (aka " << *type.aka << ")";
    }
    return os;
}

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const HoverInfo::Param& param) {
    if(param.type) {
        os << param.type->type;
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

auto hover_info(CompilationUnitRef unit, std::uint32_t offset, const HoverOptions& options)
    -> std::optional<HoverInfo> {
    /// The hover is over an include directive.
    if(auto info = include_hover(unit, offset)) {
        return info;
    }

    auto& context = unit.context();
    auto policy = hover_printing_policy(context.getPrintingPolicy());

    auto location = unit.create_location(unit.interested_file(), offset);
    auto tokens = unit.spelled_tokens_touch(location);

    /// Early exit if there were no tokens around the cursor.
    if(tokens.empty()) {
        return std::nullopt;
    }

    auto token_range = [&](const clang::syntax::Token& token) {
        auto begin = unit.file_offset(token.location());
        return LocalSourceRange(begin, begin + token.length());
    };

    /// To be used as a backup for highlighting the selected token, we use back
    /// as it aligns better with biases elsewhere (editors tend to send the
    /// position for the left of the hovered token).
    LocalSourceRange highlight_range = token_range(tokens.back());
    std::optional<HoverInfo> info;

    /// Deduced type only works on auto/decltype keywords. Note that macro
    /// hover is currently not supported: the preprocessor state is not
    /// retained in the compilation unit.
    for(const auto& token: tokens) {
        if(token.kind() == clang::tok::identifier) {
            /// Prefer the identifier token as a fallback highlighting range.
            highlight_range = token_range(token);
        } else if(token.kind() == clang::tok::kw_auto || token.kind() == clang::tok::kw_decltype) {
            if(auto deduced = ast::deduced_type(context, token.location())) {
                info = deduced_type_hover(*deduced, token, context, policy, options);
                highlight_range = token_range(token);
                break;
            }

            /// If we can't find interesting hover information for this
            /// auto/decltype keyword, return nothing to avoid showing
            /// irrelevant or incorrect information.
            return std::nullopt;
        }
    }

    /// If it wasn't auto/decltype, look for decls and expressions.
    if(!info) {
        /// Editors send the position on the left of the hovered character. So
        /// our selection tree should be biased right.
        auto tree = SelectionTree::create_right(unit, LocalSourceRange(offset, offset));
        if(const SelectionTree::Node* node = tree.common_ancestor()) {
            clang::HeuristicResolver resolver(context);
            auto decls =
                ast::explicit_reference_targets(node->data, ast::DeclRelation::Alias, &resolver);
            if(const auto* decl = pick_decl_to_use(decls)) {
                info = decl_hover(decl, policy, unit.token_buffer(), options);

                /// Layout info only shown when hovering on the field/class
                /// itself.
                if(decl == node->get<clang::Decl>()) {
                    add_layout_info(*decl, *info);
                }

                /// Look for a close enclosing expression to show the value of.
                if(!info->value) {
                    info->value = print_expr_value(node, context).printed_value;
                }

                maybe_add_callee_arg_info(node, *info, policy, options);
            } else if(const auto* expr = node->get<clang::Expr>()) {
                info = expr_hover(node, expr, context, policy, options);
            } else if(const auto* attr = node->get<clang::Attr>()) {
                info = attr_hover(attr, context);
            }
            /// FIXME: support hovers for other nodes?
            ///  - built-in types
        }
    }

    if(!info) {
        return std::nullopt;
    }

    reformat_definition(*info);
    info->symbol_range = highlight_range;

    return info;
}

auto hover(CompilationUnitRef unit,
           std::uint32_t offset,
           const HoverOptions& options,
           PositionEncoding encoding) -> std::optional<protocol::Hover> {
    auto info = hover_info(unit, offset, options);
    if(!info) {
        return std::nullopt;
    }

    return to_protocol_hover(unit, *info, options, encoding);
}

}  // namespace clice::feature
