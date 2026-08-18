/// Ported from clangd's Hover.cpp (llvmorg-21.1.8), part of the LLVM
/// project, licensed under Apache License v2.0 with LLVM Exceptions.
/// See https://llvm.org/LICENSE.txt for license information.

#include <optional>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/feature.h"
#include "semantic/decls.h"
#include "semantic/display.h"
#include "semantic/selection.h"
#include "semantic/semantics.h"
#include "semantic/symbol.h"
#include "semantic/types.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Format/Format.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Syntax/Tokens.h"

namespace clice::feature {

namespace {

using PrintedType = display::Type;
using PassType = HoverInfo::PassType;
using PassMode = HoverInfo::PassMode;

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

/// Returns the decl that should be used for querying comments: the written
/// pattern an instantiation came from, the decl itself otherwise.
auto decl_for_comment(const clang::NamedDecl* decl) -> const clang::NamedDecl* {
    /// Chase the pattern to a fixed point: a comparison operator instantiated
    /// from a spaceship operator that is itself a template instantiation
    /// needs two hops. Termination rests on clang's instantiation graph
    /// being acyclic — every hop moves strictly toward a written pattern.
    while(const auto* pattern = decls::instantiated_from(decl)) {
        if(pattern == decl) {
            break;
        }
        decl = pattern;
    }
    return decl;
}

/// Default argument might exist but be unavailable, in the case of unparsed
/// arguments for example. This function returns the default argument if it is
/// available.
/// Populates type, return_type, and parameters for function-like decls.
void fill_function_type_and_params(HoverInfo& info,
                                   const clang::Decl* decl,
                                   const clang::FunctionDecl* function,
                                   const display::Options& options) {
    info.parameters.emplace();
    for(const clang::ParmVarDecl* param: function->parameters()) {
        info.parameters->emplace_back(display::param(param, options));
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
    info.return_type = display::type(context, function->getReturnType(), options);
    clang::QualType type = function->getType();
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        /// Lambdas.
        type = var->getType().getDesugaredType(decl->getASTContext());
    }
    info.type = display::type(decl->getASTContext(), type, options);
    /// FIXME: handle variadics.
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

            if(auto value = display::expr_value(context, expr)) {
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
                const display::Options& options,
                const clang::syntax::TokenBuffer& tb) -> HoverInfo {
    HoverInfo info;
    auto& context = decl->getASTContext();

    info.access_specifier = clang::getAccessSpelling(decl->getAccess()).str();
    info.namespace_scope = display::namespace_scope(decl);
    if(!info.namespace_scope->empty()) {
        info.namespace_scope->append("::");
    }
    info.local_scope = display::local_scope(decl);
    if(!info.local_scope.empty()) {
        info.local_scope.append("::");
    }

    info.name = display::name_of(decl);
    const auto* comment_decl = decl_for_comment(decl);
    info.documentation = display::comment(comment_decl);
    if(info.documentation.empty()) {
        info.documentation = synthesize_documentation(decl);
    }

    info.kind = SymbolKind::from(decl);

    /// Fill in template params.
    if(const clang::TemplateDecl* template_decl = decl->getDescribedTemplate()) {
        info.template_parameters =
            display::template_params(template_decl->getTemplateParameters(), options);
        decl = template_decl;
    } else if(const clang::FunctionDecl* function = decl->getAsFunction()) {
        if(const auto* function_template = function->getDescribedTemplate()) {
            info.template_parameters =
                display::template_params(function_template->getTemplateParameters(), options);
            decl = function_template;
        }
    }

    /// Fill in types and params.
    if(const clang::FunctionDecl* function = underlying_function(decl)) {
        fill_function_type_and_params(info, decl, function, options);
    } else if(const auto* value = llvm::dyn_cast<clang::ValueDecl>(decl)) {
        info.type = display::type(context, value->getType(), options);
    } else if(const auto* type_param = llvm::dyn_cast<clang::TemplateTypeParmDecl>(decl)) {
        info.type = type_param->wasDeclaredWithTypename() ? "typename" : "class";
    } else if(const auto* template_param = llvm::dyn_cast<clang::TemplateTemplateParmDecl>(decl)) {
        info.type = display::template_param_type(template_param, options);
    } else if(const auto* var_template = llvm::dyn_cast<clang::VarTemplateDecl>(decl)) {
        info.type = display::type(context, var_template->getTemplatedDecl()->getType(), options);
    } else if(const auto* typedef_decl = llvm::dyn_cast<clang::TypedefNameDecl>(decl)) {
        /// TagType is not sugar, so desugaring would stop at the
        /// as-written node; canonicalize to render the underlying type
        /// fully qualified. Dependent types keep their sugar — their
        /// canonical form spells parameters as `type-parameter-N-M`.
        auto underlying = typedef_decl->getUnderlyingType();
        underlying = underlying->isDependentType() ? underlying.getDesugaredType(context)
                                                   : context.getCanonicalType(underlying);
        info.type = display::type(context, underlying, options);
    } else if(const auto* alias_template = llvm::dyn_cast<clang::TypeAliasTemplateDecl>(decl)) {
        info.type = display::type(context,
                                  alias_template->getTemplatedDecl()->getUnderlyingType(),
                                  options);
    }

    /// Fill in value with evaluated initializer if possible.
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl); var && !var->isInvalidDecl()) {
        if(const clang::Expr* init = var->getInit()) {
            info.value = display::expr_value(context, init);
        }
    } else if(const auto* enumerator = llvm::dyn_cast<clang::EnumConstantDecl>(decl)) {
        /// Dependent enums (e.g. nested in template classes) don't have values yet.
        if(!enumerator->getType()->isDependentType()) {
            info.value = llvm::toString(enumerator->getInitVal(), 10);
        }
    }

    info.definition = display::definition(decl, options, &tb);
    return info;
}

/// The standard defines __func__ as a "predefined variable".
auto predefined_expr_hover(const clang::PredefinedExpr& expr,
                           clang::ASTContext& context,
                           const display::Options& options) -> std::optional<HoverInfo> {
    HoverInfo info;
    info.name = expr.getIdentKindName();
    info.kind = SymbolKind::Variable;
    info.documentation = "Name of the current function (predefined variable)";
    if(const clang::StringLiteral* name = expr.getFunctionName()) {
        info.value.emplace();
        llvm::raw_string_ostream os(*info.value);
        name->outputString(os);
        info.type = display::type(context, name->getType(), options);
    } else {
        /// Inside templates, the approximate type `const char[]` is still useful.
        clang::QualType string_type =
            context.getIncompleteArrayType(context.CharTy.withConst(),
                                           clang::ArraySizeModifier::Normal,
                                           /*IndexTypeQuals=*/0);
        info.type = display::type(context, string_type, options);
    }
    return info;
}

auto type_as_definition(const PrintedType& type) -> std::string {
    std::string result;
    llvm::raw_string_ostream os(result);
    os << type.text;
    if(type.aka) {
        os << " // aka: " << *type.aka;
    }
    return result;
}

auto this_expr_hover(const clang::CXXThisExpr* expr,
                     clang::ASTContext& context,
                     const display::Options& options) -> std::optional<HoverInfo> {
    clang::QualType origin_this_type = expr->getType()->getPointeeType();
    clang::QualType class_type = types::declared_type(origin_this_type->getAsTagDecl());

    /// For partial specialization class, origin `this` pointee type will be
    /// parsed as `InjectedClassNameType`, which will output template arguments
    /// like "type-parameter-0-0". So we retrieve user written class type in
    /// this case.
    clang::QualType pretty_this_type = context.getPointerType(
        clang::QualType(class_type.getTypePtr(), origin_this_type.getCVRQualifiers()));

    HoverInfo info;
    info.name = "this";
    info.definition = type_as_definition(display::type(context, pretty_this_type, options));
    return info;
}

/// Generate a hover info given the deduced type.
auto deduced_type_hover(clang::QualType type,
                        const clang::syntax::Token& token,
                        clang::ASTContext& context,
                        const display::Options& options) -> HoverInfo {
    HoverInfo info;
    /// FIXME: distinguish decltype(auto) vs decltype(expr).
    info.name = clang::tok::getTokenName(token.kind());
    info.kind = SymbolKind::Type;

    if(type->isUndeducedAutoType()) {
        info.definition = "/* not deduced */";
    } else {
        info.definition = type_as_definition(display::type(context, type, options));

        if(const auto* decl = type->getAsTagDecl()) {
            const auto* comment_decl = decl_for_comment(decl);
            info.documentation = display::comment(comment_decl);
        }
    }

    return info;
}

auto string_literal_hover(const clang::StringLiteral* literal,
                          clang::ASTContext& context,
                          const display::Options& options) -> HoverInfo {
    HoverInfo info;
    info.name = "string-literal";
    info.size = (literal->getLength() + 1) * literal->getCharByteWidth() * 8;
    /// Only the printed type: the aka form is never useful for a literal.
    info.type.emplace();
    info.type->text = display::type(context, literal->getType(), options).text;
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
                               const display::Options& options) {
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

    auto parameters = decls::resolve_forwarding_params(callee);

    /// Find the argument index for the node.
    for(unsigned i = 0; i < args.size() && i < parameters.size(); ++i) {
        if(args[i] != outer.get<clang::Expr>()) {
            continue;
        }

        /// Extract matching argument from function declaration.
        if(const clang::ParmVarDecl* param = parameters[i]) {
            info.callee_arg_info.emplace(display::param(param, options));
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
                const display::Options& options) -> std::optional<HoverInfo> {
    std::optional<HoverInfo> info;

    if(const auto* literal = llvm::dyn_cast<clang::StringLiteral>(expr)) {
        /// Print the type and the size for string literals.
        info = string_literal_hover(literal, context, options);
    } else if(is_literal(expr)) {
        /// There's not much value in hovering over "42" and getting a hover
        /// card saying "42 is an int", similar for most other literals.
        /// However, if we have callee_arg_info, it's still useful to show it.
        maybe_add_callee_arg_info(node, info.emplace(), options);
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
        info = this_expr_hover(this_expr, context, options);
    }

    if(const auto* predefined = llvm::dyn_cast<clang::PredefinedExpr>(expr)) {
        info = predefined_expr_hover(*predefined, context, options);
    }

    /// For expressions we currently print the type and the value, iff it is
    /// evaluatable.
    if(auto value = display::expr_value(context, expr)) {
        info.emplace();
        info->type = display::type(context, expr->getType(), options);
        info->value = *value;
        info->name = name_for_expr(expr).str();
    }

    if(info) {
        maybe_add_callee_arg_info(node, *info, options);
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

auto file_directive_hover(CompilationUnitRef unit, std::uint32_t offset)
    -> std::optional<HoverInfo> {
    auto interested = unit.interested_file();
    auto directives_it = unit.directives().find(interested);
    if(directives_it == unit.directives().end()) {
        return std::nullopt;
    }

    auto content = unit.interested_content();
    auto* lang_opts = &unit.lang_options();

    auto file_name = [&](LocalSourceRange range) -> std::string {
        auto arg = content.substr(range.begin, range.end - range.begin).trim();
        if(arg.size() >= 2 && ((arg.front() == '"' && arg.back() == '"') ||
                               (arg.front() == '<' && arg.back() == '>'))) {
            arg = arg.drop_front().drop_back();
        }
        return arg.str();
    };

    auto line_start = content.rfind('\n', offset);
    line_start = line_start == llvm::StringRef::npos ? 0 : line_start + 1;

    auto line_end = content.find('\n', offset);
    line_end = line_end == llvm::StringRef::npos ? content.size() : line_end;

    auto try_directive = [&](clang::SourceLocation loc,
                             llvm::StringRef target) -> std::optional<HoverInfo> {
        if(target.empty())
            return std::nullopt;
        auto [fid, directive_offset] = unit.decompose_location(loc);
        /// FIXME: A directive continued with an escaped newline may have its
        /// keyword and argument on different physical lines. This check then
        /// rejects a cursor inside the argument before find_directive_argument()
        /// can match its range. Remove the same-line restriction once continued
        /// directive hover also handles the spliced argument spelling and range.
        if(fid != interested || directive_offset < line_start || directive_offset >= line_end)
            return std::nullopt;
        auto range = find_directive_argument(content, directive_offset, lang_opts);
        if(!range || offset >= range->end || offset < range->begin)
            return std::nullopt;

        HoverInfo info;
        info.name = file_name(*range);
        info.kind = SymbolKind::Header;
        info.definition = target;
        info.symbol_range = *range;
        return info;
    };

    for(const auto& include: directives_it->second.includes) {
        if(include.fid.isValid()) {
            if(auto info = try_directive(include.location, unit.file_path(include.fid))) {
                return info;
            }
        }
    }
    for(const auto& has_include: directives_it->second.has_includes) {
        if(has_include.file) {
            if(auto info = try_directive(has_include.location, unit.file_path(*has_include.file))) {
                return info;
            }
        }
    }
    for(const auto& embed: directives_it->second.embeds) {
        if(embed.file) {
            if(auto info = try_directive(embed.loc, unit.file_path(*embed.file))) {
                return info;
            }
        }
    }
    for(const auto& has_embed: directives_it->second.has_embeds) {
        if(has_embed.file) {
            if(auto info = try_directive(has_embed.loc, unit.file_path(*has_embed.file))) {
                return info;
            }
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
        auto type = context.getCanonicalTagType(record);
        if(auto size = context.getTypeSizeInCharsIfKnown(type)) {
            info.size = size->getQuantity() * 8;
        }
        if(!record->isDependentType() && record->isCompleteDefinition()) {
            info.align = context.getTypeAlign(type);
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

/// The declarations the hovered token refers to, straight from the semantic
/// map: names anchored at the token are collected from its owner chain (the
/// emitting node is always an ancestor-or-self of the token's innermost
/// owner). Rightmost touched token wins, matching the selection bias.
auto decls_at(CompilationUnitRef unit, llvm::ArrayRef<clang::syntax::Token> touched)
    -> llvm::SmallVector<const clang::NamedDecl*, 4> {
    const Semantics& semantics = unit.semantics();
    auto spelled = semantics.spelled_tokens();
    llvm::SmallVector<const clang::NamedDecl*, 4> decls;

    for(const auto& token: llvm::reverse(touched)) {
        if(should_ignore_token(token)) {
            continue;
        }

        auto it = std::ranges::partition_point(spelled, [&](const clang::syntax::Token& t) {
            return t.location() < token.location();
        });
        if(it == spelled.end() || it->location() != token.location()) {
            continue;
        }
        auto index = static_cast<std::uint32_t>(it - spelled.begin());

        llvm::DenseSet<std::uint32_t> visited;
        llvm::SmallPtrSet<const clang::NamedDecl*, 4> seen;
        for(auto owner: semantics.owners(index)) {
            for(auto n = owner; n != Semantics::invalid; n = semantics.node(n).parent) {
                /// Owners of a macro token share ancestors; scan each chain
                /// segment once.
                if(!visited.insert(n).second) {
                    break;
                }

                const SemanticNode& sem_node = semantics.node(n).node;
                if(!sem_node.is_ast()) {
                    continue;
                }

                for(auto& occurrence: resolve_occurrences(semantics, n, &unit.resolver())) {
                    auto location = occurrence.location;
                    if(location.isMacroID()) {
                        location = unit.spelling_location(location);
                    }
                    if(location == token.location() && seen.insert(occurrence.decl).second) {
                        decls.push_back(occurrence.decl);
                    }
                }
            }
        }

        if(!decls.empty()) {
            break;
        }
    }

    return decls;
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

}  // namespace

auto to_protocol_hover(const HoverInfo& info, const HoverOptions& options, const LineMap& map)
    -> protocol::Hover {
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
        result.range = to_range(map, *info.symbol_range);
    }

    return result;
}

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
            os << " (converted to " << callee_arg_info->type->text << ")";
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

auto hover_info(CompilationUnitRef unit, std::uint32_t offset, const HoverOptions& options)
    -> std::optional<HoverInfo> {
    /// The hover is over an include or embed directive.
    if(auto info = file_directive_hover(unit, offset)) {
        return info;
    }

    auto& context = unit.context();
    display::Options display_options = {
        .terse = true,
        .polish_for_declaration = true,
        .constants_as_written = true,
        .suppress_ctor_template_args = true,
        .resolve_decltype = true,
        .tag_keyword_prefix = true,
        .show_aka = options.show_aka,
    };

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
            if(auto deduced = types::deduced_type(context, token.location())) {
                info = deduced_type_hover(*deduced, token, context, display_options);
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
            auto targets = decls_at(unit, tokens);
            if(const auto* decl = pick_decl_to_use(targets)) {
                info = decl_hover(decl, display_options, unit.token_buffer());

                /// Layout info only shown when hovering on the field/class
                /// itself.
                if(decl == node->get<clang::Decl>()) {
                    add_layout_info(*decl, *info);
                }

                /// Look for a close enclosing expression to show the value of.
                if(!info->value) {
                    info->value = print_expr_value(node, context).printed_value;
                }

                maybe_add_callee_arg_info(node, *info, display_options);
            } else if(const auto* expr = node->get<clang::Expr>()) {
                info = expr_hover(node, expr, context, display_options);
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

    LineMap map(unit.interested_content(), unit.line_starts(), encoding);
    return to_protocol_hover(*info, options, map);
}

}  // namespace clice::feature
