module;

#include "support/prelude.h"

module clice;

import :compile.compilation;
import :feature.document_links;
import :feature.documentation;
import :feature.hover;
import :feature.protocol;
import :index.peer_queries;
import :index.query;
import :index.symbol;
import :index.text;
import :semantic.decls;
import :semantic.display;
import :semantic.resolver;
import :semantic.selection;
import :semantic.semantics;
import :semantic.symbol;
import :semantic.types;
import :support.text;
import :syntax.documentation;
import :syntax.lexer;
import :syntax.lexical_scan;
import :vfs.file_table;

namespace clice::feature {

namespace {

using PassMode = HoverInfo::PassMode;

/// What one request prints its card with.
struct Request {
    CompilationUnit& unit;
    const DocumentationOptions& documentation;
    const display::Options& display;
};

/// The function a call of the declaration runs: its own, or the call
/// operator of the lambda a variable holds or refers to (not one it points
/// to).
auto underlying_function(const clang::Decl* decl) -> const clang::FunctionDecl* {
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        if(const auto* record = var->getType().getNonReferenceType()->getAsCXXRecordDecl()) {
            return record->getLambdaCallOperator();
        }
    }
    return decl->getAsFunction();
}

/// The field `expr` names through `this`, casts aside.
auto own_field(const clang::Expr* expr) -> const clang::FieldDecl* {
    const auto* member = llvm::dyn_cast<clang::MemberExpr>(expr->IgnoreCasts());
    if(!member || !llvm::isa<clang::CXXThisExpr>(member->getBase()->IgnoreCasts())) {
        return nullptr;
    }
    const auto* field = llvm::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
    return field && field->getDeclName().isIdentifier() ? field : nullptr;
}

/// The field a getter returns: `T value() { return value_; }`.
auto returned_field(const clang::CXXMethodDecl& method, const clang::CompoundStmt& body)
    -> const clang::FieldDecl* {
    if(method.getNumParams() != 0 || method.isVariadic() || body.size() != 1) {
        return nullptr;
    }
    const auto* returned = llvm::dyn_cast<clang::ReturnStmt>(body.body_front());
    return returned && returned->getRetValue() ? own_field(returned->getRetValue()) : nullptr;
}

/// The field a setter assigns its parameter to, moved or not, before it
/// returns nothing, `this` or `*this`: `void set(T value) { value_ = value; }`.
auto assigned_field(const clang::CXXMethodDecl& method, const clang::CompoundStmt& body)
    -> const clang::FieldDecl* {
    if(method.isConst() || method.getNumParams() != 1 || method.isVariadic() ||
       method.getParamDecl(0)->isParameterPack() || body.size() == 0 || body.size() > 2) {
        return nullptr;
    }

    if(body.size() == 2) {
        const auto* returned = llvm::dyn_cast<clang::ReturnStmt>(body.body_back());
        const auto* value =
            returned && returned->getRetValue() ? returned->getRetValue()->IgnoreCasts() : nullptr;
        if(const auto* deref = llvm::dyn_cast_if_present<clang::UnaryOperator>(value);
           deref && deref->getOpcode() == clang::UO_Deref) {
            value = deref->getSubExpr()->IgnoreCasts();
        }
        if(!llvm::isa_and_present<clang::CXXThisExpr>(value)) {
            return nullptr;
        }
    }

    const clang::Expr* target = nullptr;
    const clang::Expr* source = nullptr;
    if(const auto* assign = llvm::dyn_cast<clang::BinaryOperator>(body.body_front());
       assign && assign->getOpcode() == clang::BO_Assign) {
        target = assign->getLHS();
        source = assign->getRHS();
    } else if(const auto* call = llvm::dyn_cast<clang::CXXOperatorCallExpr>(body.body_front());
              call && call->getOperator() == clang::OO_Equal && call->getNumArgs() == 2) {
        target = call->getArg(0);
        source = call->getArg(1);
    } else {
        return nullptr;
    }

    source = source->IgnoreCasts();
    if(const auto* call = llvm::dyn_cast<clang::CallExpr>(source)) {
        const auto* callee = llvm::dyn_cast_if_present<clang::NamedDecl>(call->getCalleeDecl());
        if(call->getNumArgs() != 1 || !callee || !callee->getIdentifier() ||
           callee->getName() != "move" || !callee->isInStdNamespace()) {
            return nullptr;
        }
        source = call->getArg(0)->IgnoreCasts();
    }
    const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(source);
    return ref && ref->getDecl() == method.getParamDecl(0) ? own_field(target) : nullptr;
}

/// What a method without a comment of its own evidently does: return a
/// field, or set one from its parameter.
auto accessor_description(const clang::NamedDecl* decl) -> std::string {
    const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(decl);
    if(!method || !method->getDeclName().isIdentifier() || method->isStatic()) {
        return {};
    }
    const auto* definition =
        llvm::dyn_cast_if_present<clang::CXXMethodDecl>(method->getDefinition());
    // A deleted method has no body, nor has one whose body is deferred.
    const auto* body = definition
                           ? llvm::dyn_cast_if_present<clang::CompoundStmt>(definition->getBody())
                           : nullptr;
    if(!body) {
        return {};
    }
    if(const auto* field = returned_field(*definition, *body)) {
        return llvm::formatv("Trivial accessor for `{0}`.", field->getName()).str();
    }
    if(const auto* field = assigned_field(*definition, *body)) {
        return llvm::formatv("Trivial setter for `{0}`.", field->getName()).str();
    }
    return {};
}

/// A value's type inside a template as far as the resolver reaches it: a
/// generic lambda's `auto` parameter is the type its only call passes.
auto resolved_type(const clang::ValueDecl* value, CompilationUnit& unit) -> clang::QualType {
    auto type = value->getType();
    if(type.isNull() || !type->isDependentType()) {
        return type;
    }
    /// The instantiated parameter keeps the aliases the call deduced; the
    /// resolver works from canonical template arguments.
    if(const auto* param = llvm::dyn_cast<clang::ParmVarDecl>(value)) {
        // The helper keeps its clangd signature, which speaks mutable
        // pointers; it only reads through them.
        if(auto* instantiated = decls::only_instantiation(const_cast<clang::ParmVarDecl*>(param))) {
            return instantiated->getType();
        }
    }
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(value);
       var && (var->getInit() || var->isCXXForRangeDecl()) && type->getContainedAutoType()) {
        auto deduced = unit.get<types::TemplateResolver>().deduce(var);
        return deduced.isNull() ? type : deduced;
    }
    return unit.get<types::TemplateResolver>().resolve(type);
}

/// What the `auto` of a variable a dependent initializer leaves undeduced
/// stands for, as far as the resolver reaches it and code could write it:
/// the variable's resolved type with the declarator's pointers, references
/// and qualifiers taken off.
auto resolved_auto(const clang::VarDecl* var, CompilationUnit& unit) -> clang::QualType {
    auto resolved = var->getInit() || var->isCXXForRangeDecl()
                        ? unit.get<types::TemplateResolver>().deduce(var)
                        : clang::QualType();
    if(resolved.isNull() || !types::writable(resolved)) {
        return {};
    }
    auto declared = var->getType();
    while(!llvm::isa<clang::AutoType>(declared.getTypePtr())) {
        // `auto&&` bound to an lvalue stands for the lvalue reference.
        if(declared->isRValueReferenceType() && resolved->isLValueReferenceType()) {
            return resolved;
        }
        if(!llvm::isa<clang::PointerType, clang::ReferenceType>(declared.getTypePtr())) {
            return {};
        }
        declared = declared->getPointeeType();
        resolved = resolved->getPointeeType();
        if(resolved.isNull()) {
            return {};
        }
    }
    return unit.context().getQualifiedType(resolved.getLocalUnqualifiedType(),
                                           resolved.getLocalQualifiers() -
                                               declared.getLocalQualifiers());
}

/// The entity a template declares, the declaration itself otherwise: it
/// holds the kind and the attributes.
auto templated(const clang::NamedDecl* decl) -> const clang::NamedDecl* {
    if(const auto* template_decl = llvm::dyn_cast<clang::TemplateDecl>(decl);
       template_decl && template_decl->getTemplatedDecl()) {
        return template_decl->getTemplatedDecl();
    }
    return decl;
}

/// The kind a card names its declaration by. Unlike a token's kind, an
/// operator function is a function or a method like any other.
auto card_kind(const clang::NamedDecl* decl) -> SymbolKind {
    if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
        return function->isCXXClassMember() ? SymbolKind::Method : SymbolKind::Function;
    }
    /// `using typename Base<T>::name` names a type the base will declare.
    if(llvm::isa<clang::UnresolvedUsingTypenameDecl>(decl)) {
        return SymbolKind::Type;
    }
    return SymbolKind::from(decl);
}

/// The documentation of a parameter: what the comment of its function says
/// of it, or of its template for a template parameter.
auto parameter_documentation(CompilationUnit& unit,
                             const clang::NamedDecl* decl,
                             const DocumentationOptions& options) -> std::optional<Documentation> {
    bool function_parameter = llvm::isa<clang::ParmVarDecl>(decl);
    if(!function_parameter && !decl->isTemplateParameter()) {
        return std::nullopt;
    }
    const auto* owner =
        llvm::dyn_cast<clang::NamedDecl>(clang::Decl::castFromDeclContext(decl->getDeclContext()));
    if(!owner) {
        return std::nullopt;
    }
    auto owner_documentation = documentation(unit, owner, options);
    const Documentation::Param* param = nullptr;
    if(!function_parameter) {
        param = owner_documentation.template_param(decl->getName());
    } else {
        for(const auto* named: decls::named_parameters(*llvm::cast<clang::ParmVarDecl>(decl))) {
            if((param = owner_documentation.param(named->getName()))) {
                break;
            }
        }
    }
    if(!param) {
        return std::nullopt;
    }
    return Documentation{.brief = param->text};
}

/// The non-inline namespaces from the global scope down to `ns`, joined
/// with `::`; an anonymous one is `(anonymous)`, its names being local to
/// their file.
auto namespace_path(const clang::NamespaceDecl& ns) -> std::string {
    llvm::SmallVector<llvm::StringRef> names;
    for(const clang::DeclContext* scope = &ns; scope; scope = scope->getParent()) {
        if(const auto* outer = llvm::dyn_cast<clang::NamespaceDecl>(scope);
           outer && (!outer->isInline() || outer->isAnonymousNamespace())) {
            names.push_back(outer->isAnonymousNamespace() ? "(anonymous)" : outer->getName());
        }
    }
    return llvm::join(views::reverse(names), "::");
}

/// Where a declaration is: the kind and qualified name of the namespace,
/// class, enum or function around it, empty at global scope.
struct Scope {
    std::string kind;
    std::string name;
};

auto scope_of(const clang::Decl* decl, const display::Options& options) -> Scope {
    for(const auto* context = decl->getDeclContext(); context; context = context->getParent()) {
        if(const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
            /// Code names an inline namespace's members through the namespace
            /// around it.
            if(ns->isInline() && !ns->isAnonymousNamespace()) {
                continue;
            }
            return {.kind = "namespace", .name = namespace_path(*ns)};
        }

        const auto* lambda = llvm::dyn_cast<clang::CXXRecordDecl>(context);
        if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(context)) {
            lambda = method->getParent();
        }
        if(lambda && lambda->isLambda()) {
            auto outer = scope_of(lambda, options);
            if(outer.kind.empty()) {
                return {.kind = "lambda"};
            }
            return {.kind = "lambda in " + outer.kind, .name = std::move(outer.name)};
        }

        if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(context)) {
            /// The members of an anonymous struct or union are named as the
            /// class's around it.
            if(const auto* record = llvm::dyn_cast<clang::RecordDecl>(tag);
               record && record->isAnonymousStructOrUnion()) {
                continue;
            }
            /// An unnamed one is named by where it is, as a lambda is.
            if(!tag->getDeclName() && !tag->getTypedefNameForAnonDecl()) {
                auto outer = scope_of(tag, options);
                auto kind = "unnamed " + tag->getKindName().str();
                if(outer.kind.empty()) {
                    return {.kind = std::move(kind)};
                }
                return {.kind = kind + " in " + outer.kind, .name = std::move(outer.name)};
            }
            auto plain = options;
            plain.show_aka = false;
            /// C spells a tag with its keyword, which the kind already says.
            auto name =
                tag->getDeclName() && tag->getASTContext().getLangOpts().CPlusPlus
                    ? display::type(tag->getASTContext(), types::declared_type(tag), plain).text
                    : display::name_of(tag);
            return {.kind = tag->getKindName().str(), .name = std::move(name)};
        }

        if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(context)) {
            /// Without the inline namespaces code does not write, as
            /// namespace_path() leaves them out.
            clang::PrintingPolicy policy(function->getASTContext().getLangOpts());
            policy.SuppressInlineNamespace =
                llvm::to_underlying(clang::PrintingPolicy::SuppressInlineNamespaceMode::All);
            std::string name;
            llvm::raw_string_ostream os(name);
            function->printQualifiedName(os, policy);
            return {.kind = kind_noun(card_kind(function)).str(), .name = std::move(name)};
        }
    }
    return {};
}

/// The name a card's title shows: a specialization's with its arguments,
/// a nameless entity's as presented (`(anonymous struct)`, `(lambda)`).
auto title_name(const clang::NamedDecl* decl, const display::Options& options) -> std::string {
    auto name = display::name_of(decl, {.suppress_ctor_template_args = true, .qualified = false});
    if(name.empty()) {
        return display::name_of(decl);
    }
    if(llvm::isa<clang::ClassTemplateSpecializationDecl, clang::VarTemplateSpecializationDecl>(
           decl)) {
        return name + display::specialization_args(*decl, options);
    }
    return name + display::template_args(*decl);
}

/// The declarations a using-declaration brings in that the code declares,
/// without the copy and move constructors an inheriting one names too.
auto brought_in(const clang::UsingDecl& using_decl) -> llvm::SmallVector<const clang::NamedDecl*> {
    llvm::SmallVector<const clang::NamedDecl*> targets;
    for(const auto* shadow: using_decl.shadows()) {
        if(!shadow->getTargetDecl()->isImplicit()) {
            targets.push_back(shadow->getTargetDecl());
        }
    }
    return targets;
}

/// Whether a written type spells the type it declares: no placeholder,
/// `decltype` or `typeof`, and no `auto` an abbreviated template turned into
/// a parameter of its own.
bool spelled(clang::TypeLoc written) {
    const auto* type = types::unwrap(written).getTypePtr();
    if(llvm::isa<clang::DeducedType, clang::DecltypeType, clang::TypeOfExprType, clang::TypeOfType>(
           type)) {
        return false;
    }
    const auto* param = llvm::dyn_cast<clang::TemplateTypeParmType>(type);
    return !param || !param->getDecl() || !param->getDecl()->isImplicit();
}

/// Whether the declaration of a value spells its type. An enumerator's is
/// the enum the title names.
bool spells_type(const clang::ValueDecl& value) {
    if(llvm::isa<clang::EnumConstantDecl>(value)) {
        return true;
    }
    const auto* declarator = llvm::dyn_cast<clang::DeclaratorDecl>(&value);
    return declarator && declarator->getTypeSourceInfo() &&
           spelled(declarator->getTypeSourceInfo()->getTypeLoc());
}

/// Note what an alias in the declaration stands for, once per alias: the
/// alias alone, without the pointers, references, arrays and qualifiers
/// around it.
void add_alias(HoverInfo& info,
               clang::ASTContext& context,
               clang::QualType type,
               const display::Options& options) {
    while(true) {
        /// A parameter of array or function type is adjusted to a pointer.
        if(const auto* adjusted = llvm::dyn_cast<clang::AdjustedType>(type.getTypePtr())) {
            type = adjusted->getOriginalType();
        } else if(const auto* pointer = llvm::dyn_cast<clang::PointerType>(type.getTypePtr())) {
            type = pointer->getPointeeType();
        } else if(const auto* reference = llvm::dyn_cast<clang::ReferenceType>(type.getTypePtr())) {
            type = reference->getPointeeType();
        } else if(const auto* array = llvm::dyn_cast<clang::ArrayType>(type.getTypePtr())) {
            type = array->getElementType();
        } else {
            break;
        }
    }
    auto shown = display::type(context, type.getUnqualifiedType(), options);
    if(shown.aka && ranges::none_of(info.aliases, [&](const display::Type& known) {
           return known.text == shown.text;
       })) {
        info.aliases.push_back(std::move(shown));
    }
}

/// The facts of a function: the aliases its signature spells, and its type
/// when it is a lambda or names its return type through an expression.
void add_function(HoverInfo& info,
                  const clang::Decl* decl,
                  const clang::FunctionDecl& function,
                  const display::Options& options) {
    auto& context = function.getASTContext();
    for(const auto* param: function.parameters()) {
        add_alias(info, context, param->getType(), options);
    }

    /// The name of a constructor, a destructor or a conversion holds the
    /// type it gives.
    auto name_kind = function.getDeclName().getNameKind();
    if(name_kind == clang::DeclarationName::CXXConstructorName ||
       name_kind == clang::DeclarationName::CXXDestructorName ||
       name_kind == clang::DeclarationName::CXXConversionFunctionName) {
        return;
    }
    /// Clang prints the type a placeholder deduces in place of the
    /// placeholder.
    auto returned = function.getFunctionTypeLoc();
    if(llvm::isa<clang::VarDecl>(decl) || (returned && !spelled(returned.getReturnLoc()) &&
                                           !function.getReturnType()->getContainedDeducedType())) {
        /// A lambda's call operator is `const`, which says nothing of calling
        /// it. A C function may have no prototype, `int f()`.
        auto type = function.getType();
        if(const auto* prototype = type->getAs<clang::FunctionProtoType>()) {
            auto extra = prototype->getExtProtoInfo();
            extra.TypeQuals = {};
            extra.RefQualifier = clang::RQ_None;
            type = context.getFunctionType(function.getReturnType(),
                                           prototype->getParamTypes(),
                                           extra);
        }
        info.type = display::type(context, type, options);
    } else {
        add_alias(info, context, function.getReturnType(), options);
    }
}

/// The type of a structured binding of a tuple-like object, as the reader
/// knows it rather than as `std::tuple_element<0, T>::type`.
auto binding_type(const clang::BindingDecl& binding) -> clang::QualType {
    auto type = binding.getType();
    // A binding of a declaration in error has none.
    if(type.isNull()) {
        return type;
    }
    if(const auto* member = llvm::dyn_cast<clang::TypedefType>(type.getTypePtr());
       member && member->getDecl()->getDeclContext()->isRecord()) {
        return binding.getASTContext().getQualifiedType(member->desugar(),
                                                        type.getLocalQualifiers());
    }
    return type;
}

/// The source text of a declaration, for one clang could not make sense of:
/// it prints the recovered declaration, as `int x` for `Unknown x`.
auto written_text(CompilationUnit& unit, const clang::Decl* decl) -> std::optional<std::string> {
    auto range = unit.written_range(decls::written_range(decl));
    if(!range) {
        return std::nullopt;
    }
    auto text = unit.file_content(range->file)
                    .substr(range->range.begin, range->range.end - range->range.begin)
                    .rtrim();
    /// An initializer clang drops leaves its `=` behind.
    if(text.consume_back("=")) {
        text = text.rtrim();
    }
    return text.str();
}

/// Lay out code a card shows, a printed declaration or an expansion, in the
/// LLVM style: it is printed as one line, not as anyone wrote it.
void reformat(std::string& code) {
    if(code.empty()) {
        return;
    }
    auto replacements = clang::format::reformat(clang::format::getLLVMStyle(),
                                                code,
                                                {clang::tooling::Range(0, code.size())});
    auto formatted = clang::tooling::applyAllReplacements(code, replacements);
    if(!formatted) {
        llvm::consumeError(formatted.takeError());
        return;
    }
    code = std::move(*formatted);
}

/// A declaration as its card shows it, laid out: the attributes written
/// before it on a line of their own, after its template head.
auto layout(display::Definition definition, bool exported) -> std::string {
    std::string prefix = exported ? "export " : "";
    if(definition.attributes.empty()) {
        auto text = prefix + definition.head + definition.text;
        reformat(text);
        return text;
    }
    std::string text;
    if(!definition.head.empty()) {
        auto head = prefix + definition.head;
        reformat(head);
        text = head + "\n";
        prefix.clear();
    }
    auto attributes = prefix + definition.attributes;
    reformat(attributes);
    reformat(definition.text);
    return text + attributes + "\n" + definition.text;
}

/// Note the attribute macros a definition shows, once each, with what they
/// expand to.
void add_macros(HoverInfo& info,
                const display::Definition& definition,
                const display::Options& options) {
    if(!options.show_aka) {
        return;
    }
    for(const auto& macro: definition.attribute_macros) {
        if(ranges::none_of(info.attribute_macros, [&](const display::AttributeMacro& known) {
               return known.use == macro.use;
           })) {
            info.attribute_macros.push_back(macro);
        }
    }
}

/// The dependent specialization the code at `node` names a member of a
/// class template through: `vector<Point<T>>` for `pts.push_back` or
/// `typename vector<Point<T>>::value_type`.
auto accessed_specialization(const SelectionTree::Node& node, CompilationUnit& unit)
    -> const clang::TemplateSpecializationType* {
    if(const auto* access = node.get<clang::CXXDependentScopeMemberExpr>()) {
        return types::accessed_specialization(*access, unit.get<types::TemplateResolver>());
    }
    const auto* loc = node.get<clang::TypeLoc>();
    const auto* name = loc ? loc->getType()->getAs<clang::DependentNameType>() : nullptr;
    if(!name || name->getQualifier().getKind() != clang::NestedNameSpecifier::Kind::Type) {
        return nullptr;
    }
    return name->getQualifier().getAsType()->getAs<clang::TemplateSpecializationType>();
}

/// The card of a declaration.
auto decl_hover(const clang::NamedDecl* decl, const Request& request) -> HoverInfo {
    HoverInfo info;
    auto& unit = request.unit;
    auto& context = decl->getASTContext();
    const auto& options = request.display;
    const auto* entity = templated(decl);
    /// The types the card states, named as code in the declaration's scope
    /// names them.
    auto shown = options;
    shown.scope = decl->getDeclContext();
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
        shown.source = var->getInit();
    } else if(const auto* binding = llvm::dyn_cast<clang::BindingDecl>(decl)) {
        const auto* decomposed = llvm::cast<clang::VarDecl>(binding->getDecomposedDecl());
        shown.source = decomposed->getInit();
    }

    if(auto access = decl->getAccess();
       access == clang::AS_private || access == clang::AS_protected) {
        info.access = clang::getAccessSpelling(access).str();
    }
    auto scope = scope_of(decl, options);
    info.scope_kind = std::move(scope.kind);
    info.scope = std::move(scope.name);
    info.name = title_name(decl, options);
    info.kind = card_kind(entity);

    if(auto documented = parameter_documentation(unit, decl, request.documentation)) {
        info.documentation = std::move(*documented);
    } else {
        info.documentation = documentation(unit, decl, request.documentation);
    }
    if(info.documentation.empty()) {
        info.documentation.brief = accessor_description(decl);
    }
    if(std::string message; entity->isDeprecated(&message)) {
        info.deprecation = std::move(message);
    }
    if(const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(entity)) {
        for(const auto* overridden: method->overridden_methods()) {
            info.overrides.push_back(overridden->getQualifiedNameAsString());
        }
    }

    /// A template prints from its `template` keyword on.
    const clang::Decl* declaration = decl;
    if(const auto* described = decl->getDescribedTemplate()) {
        declaration = described;
    } else if(const auto* function = decl->getAsFunction()) {
        if(const auto* described = function->getDescribedTemplate()) {
            declaration = described;
        }
    }

    if(const auto* using_decl = llvm::dyn_cast<clang::UsingDecl>(decl)) {
        /// The declarations it brings in, in the order they are declared,
        /// as many as a class's members at most.
        auto targets = brought_in(*using_decl);
        auto& sources = context.getSourceManager();
        ranges::sort(targets, [&](const clang::NamedDecl* left, const clang::NamedDecl* right) {
            return sources.isBeforeInTranslationUnit(left->getLocation(), right->getLocation());
        });
        /// The using-declaration of inheriting constructors is named after
        /// the class it is in.
        if(!targets.empty()) {
            info.kind = card_kind(templated(targets.front()));
            info.name = title_name(targets.front(), options);
        }
        for(auto [index, target]: views::enumerate(targets)) {
            if(static_cast<std::size_t>(index) == options.max_members) {
                info.target_definition += "\n// ...";
                break;
            }
            if(index != 0) {
                info.target_definition += '\n';
            }
            auto definition = display::definition(target, options, [&](clang::SourceRange range) {
                return unit.focus_file().tokens().expanded(range).size();
            });
            add_macros(info, definition, options);
            info.target_definition += layout(std::move(definition), false) + ";";
        }
    } else if(const auto* function = underlying_function(entity)) {
        add_function(info, entity, *function, shown);
    } else if(const auto* binding = llvm::dyn_cast<clang::BindingDecl>(entity)) {
        if(auto type = binding_type(*binding); !type.isNull()) {
            info.type = display::type(context, type, shown);
        }
    } else if(const auto* value = llvm::dyn_cast<clang::ValueDecl>(entity)) {
        auto type = resolved_type(value, unit);
        if(!spells_type(*value)) {
            info.type = display::type(context, type, shown);
        } else if(type != value->getType()) {
            /// The resolver named what a dependent type stands for.
            info.aliases.push_back({display::type(context, value->getType(), shown).text,
                                    display::type(context, type, shown).text});
        } else if(!llvm::isa<clang::EnumConstantDecl>(value)) {
            add_alias(info, context, type, shown);
        }
    } else if(const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(entity)) {
        /// The alias's own target: through the readable types, a standard
        /// alias would name itself.
        auto plain = shown;
        plain.readable = nullptr;
        add_alias(info, context, alias->getUnderlyingType(), plain);
        const auto* tag = alias->getUnderlyingType()->getAsTagDecl();
        if(tag && tag->getDefinition() && tag->getTypedefNameForAnonDecl() != alias &&
           !llvm::isa<clang::ClassTemplateSpecializationDecl>(tag)) {
            auto definition =
                display::definition(tag->getDefinition(), options, [&](clang::SourceRange range) {
                    return unit.focus_file().tokens().expanded(range).size();
                });
            add_macros(info, definition, options);
            info.target_definition = layout(std::move(definition), false);
        }
    }

    /// A parameter's value is the caller's, not its default argument.
    if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl);
       var && !var->isInvalidDecl() && !llvm::isa<clang::ParmVarDecl>(var)) {
        if(const clang::Expr* init = var->getInit()) {
            info.value = display::expr_value(context, init);
        }
    } else if(const auto* enumerator = llvm::dyn_cast<clang::EnumConstantDecl>(decl)) {
        /// Dependent enums (e.g. nested in template classes) don't have values yet.
        if(!enumerator->getType()->isDependentType()) {
            info.value = llvm::toString(enumerator->getInitVal(), 10);
        }
    }

    /// A structured binding is declared by the whole declaration.
    if(const auto* binding = llvm::dyn_cast<clang::BindingDecl>(decl)) {
        declaration = binding->getDecomposedDecl();
    }
    if(declaration->isInvalidDecl()) {
        if(auto written = written_text(unit, declaration)) {
            info.definition = std::move(*written);
            if(decls::is_exported(decl)) {
                info.definition.insert(0, "export ");
            }
            reformat(info.definition);
            return info;
        }
    }
    auto definition = display::definition(declaration, options, [&](clang::SourceRange range) {
        return unit.focus_file().tokens().expanded(range).size();
    });
    add_macros(info, definition, options);
    info.definition = layout(std::move(definition), decls::is_exported(decl));
    return info;
}

/// The bits of a class's size its fields, its bases' and its virtual table
/// pointer take; a field of class type takes all of its own.
auto used_bits(const clang::RecordDecl& record, const clang::ASTContext& context) -> std::uint64_t {
    std::uint64_t used = 0;
    for(const auto* field: record.fields()) {
        std::uint64_t size = 0;
        if(field->isBitField()) {
            size = field->getBitWidthValue();
        } else if(auto known = context.getTypeSizeInCharsIfKnown(field->getType());
                  known && !field->isZeroSize(context)) {
            /// A flexible array member has no size.
            size = context.toBits(*known);
        }
        used = record.isUnion() ? std::max(used, size) : used + size;
    }
    const auto* cxx = llvm::dyn_cast<clang::CXXRecordDecl>(&record);
    if(!cxx) {
        return used;
    }
    for(const auto& base: cxx->bases()) {
        used += used_bits(*base.getType()->getAsCXXRecordDecl(), context);
    }
    if(context.getASTRecordLayout(cxx).hasOwnVFPtr()) {
        used += context.getTargetInfo().getPointerWidth(clang::LangAS::Default);
    }
    return used;
}

/// How the compiler lays out a class, an alias of one or a field: sizes,
/// alignment, a field's offset and the padding after it, a class's padding
/// in all.
void add_layout(const clang::NamedDecl& decl, HoverInfo& info) {
    if(decl.isInvalidDecl()) {
        return;
    }
    const auto& context = decl.getASTContext();

    const auto* record = llvm::dyn_cast<clang::RecordDecl>(&decl);
    if(const auto* alias = llvm::dyn_cast<clang::TypedefNameDecl>(&decl)) {
        record = alias->getUnderlyingType()->getAsRecordDecl();
    }
    if(record) {
        record = record->getDefinition();
        if(!record || record->isInvalidDecl() || record->isDependentType()) {
            return;
        }
        auto type = context.getCanonicalTagType(record);
        info.size = context.getTypeSize(type);
        info.align = context.getTypeAlign(type);
        /// Virtual bases sit wherever the most derived class puts them.
        const auto* cxx = llvm::dyn_cast<clang::CXXRecordDecl>(record);
        if(auto used = used_bits(*record, context);
           used != 0 && used < *info.size && (!cxx || cxx->getNumVBases() == 0)) {
            info.padding = *info.size - used;
        }
        return;
    }

    const auto* field = llvm::dyn_cast<clang::FieldDecl>(&decl);
    const auto* parent = field ? field->getParent()->getDefinition() : nullptr;
    if(!parent || parent->isInvalidDecl() || parent->isDependentType()) {
        return;
    }
    const auto& layout = context.getASTRecordLayout(parent);
    auto offset = layout.getFieldOffset(field->getFieldIndex());
    info.align = context.getTypeAlign(field->getType());
    /// A member of an anonymous struct or union is one of the class around
    /// it, as far into that class as the anonymous member is. Every member
    /// of a union is at its start.
    auto outer_offset = offset;
    bool in_unions = parent->isUnion();
    for(const auto* outer = parent; outer->isAnonymousStructOrUnion();) {
        /// A function's or a namespace's anonymous union is no member.
        const auto* holder = llvm::dyn_cast<clang::RecordDecl>(outer->getParent());
        if(!holder) {
            break;
        }
        auto fields = holder->fields();
        const auto* member = *ranges::find_if(fields, [&](const clang::FieldDecl* field) {
            return field->getType()->getAsRecordDecl() == outer;
        });
        outer_offset += context.getASTRecordLayout(holder).getFieldOffset(member->getFieldIndex());
        in_unions = in_unions && holder->isUnion();
        outer = holder;
    }
    if(!in_unions) {
        info.offset = outer_offset;
    }
    if(field->isBitField()) {
        info.size = field->getBitWidthValue();
    } else if(auto size = context.getTypeSizeInCharsIfKnown(field->getType())) {
        info.size = field->isZeroSize(context) ? 0 : context.toBits(*size);
    }
    if(!info.size) {
        return;
    }
    /// Up to the next field, which may be a bitfield, or to the end.
    auto end = offset + *info.size;
    if(!parent->isUnion() && field->getFieldIndex() + 1 < layout.getFieldCount()) {
        if(auto next = layout.getFieldOffset(field->getFieldIndex() + 1); next >= end) {
            info.padding = next - end;
        }
    } else {
        info.padding = context.toBits(layout.getSize()) - end;
    }
}

/// The value the hovered name stands for, when it is known at compile
/// time: a variable's or an enumerator's own; for a function, the result of
/// the call it is the callee of, or of the operator call its token spells.
auto own_value(const SelectionTree::Node& node,
               const clang::NamedDecl& decl,
               const clang::ASTContext& context) -> std::optional<std::string> {
    const auto& outer = node.outer_implicit();
    const auto* expr = outer.get<clang::Expr>();
    if(!expr || !llvm::isa<clang::ValueDecl, clang::ConceptDecl>(decl)) {
        return std::nullopt;
    }
    if(underlying_function(&decl) &&
       llvm::isa<clang::DeclRefExpr, clang::MemberExpr>(expr->IgnoreParenImpCasts())) {
        const auto* call = outer.parent ? outer.parent->get<clang::CallExpr>() : nullptr;
        if(!call || call->getCallee()->IgnoreParenImpCasts() != expr->IgnoreParenImpCasts()) {
            return std::nullopt;
        }
        expr = call;
    }
    return display::expr_value(context, expr);
}

/// Whether an implicit conversion between the hovered argument and the call
/// changes its type: a derived class to its base, a decay or a copy does
/// not.
bool converts(const SelectionTree::Node& node, const SelectionTree::Node& argument) {
    for(const auto* step = node.parent; step && step != argument.parent; step = step->parent) {
        if(const auto* cast = step->get<clang::ImplicitCastExpr>()) {
            switch(cast->getCastKind()) {
                case clang::CK_NoOp:
                case clang::CK_LValueToRValue:
                case clang::CK_DerivedToBase:
                case clang::CK_UncheckedDerivedToBase:
                case clang::CK_ArrayToPointerDecay:
                case clang::CK_FunctionToPointerDecay:
                case clang::CK_NullToPointer:
                case clang::CK_NullToMemberPointer: break;
                default: return true;
            }
        } else if(const auto* construct = step->get<clang::CXXConstructExpr>();
                  construct && !construct->getConstructor()->isCopyOrMoveConstructor()) {
            return true;
        }
    }
    return false;
}

/// For an argument of a call, the parameter it is passed to — the one a
/// pack forwards it to — how, and the type it is converted to on the way.
/// The operands of an overloaded operator are left out, a call operator's
/// arguments aside: what they are passed as says little.
void add_pass(const SelectionTree::Node& node, HoverInfo& info, const display::Options& options) {
    const auto& argument = node.outer_implicit();
    const auto* call = argument.parent ? argument.parent->get<clang::Stmt>() : nullptr;
    if(!call) {
        return;
    }
    if(const auto* operator_call = llvm::dyn_cast<clang::CXXOperatorCallExpr>(call);
       operator_call && operator_call->getOperator() != clang::OO_Call) {
        return;
    }
    const auto* expr = argument.get<clang::Expr>();
    for(const auto& binding: decls::parameter_bindings(call)) {
        if(binding.argument != expr) {
            continue;
        }
        const auto* param = binding.forwarded;
        auto type = param->getType();
        auto shown = options;
        shown.scope = param->getDeclContext();
        info.callee_arg_info = display::param(param, shown);
        if(!type->isReferenceType()) {
            info.pass_mode = PassMode::Value;
        } else if(type->isRValueReferenceType()) {
            info.pass_mode = PassMode::RValueRef;
        } else {
            info.pass_mode =
                type.getNonReferenceType().isConstQualified() ? PassMode::ConstRef : PassMode::Ref;
        }
        if(converts(node, argument)) {
            info.converted_to = display::type(param->getASTContext(),
                                              type.getNonReferenceType().getUnqualifiedType(),
                                              shown)
                                    .text;
        }
        return;
    }
}

/// The text of the literal, cut short past 40 characters.
auto literal_text(CompilationUnit& unit, const clang::Expr& literal) -> std::string {
    constexpr std::size_t limit = 40;
    auto range = unit.written_range(literal.getSourceRange());
    if(!range) {
        return "literal";
    }
    auto text = unit.file_content(range->file)
                    .substr(range->range.begin, range->range.end - range->range.begin);
    if(text.size() <= limit) {
        return text.str();
    }
    /// Not inside a UTF-8 sequence.
    auto cut = limit;
    while(cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        cut -= 1;
    }
    return text.take_front(cut).str() + "...";
}

/// The card of a literal: its type and value, a string's length and size.
/// A literal of no number, character or string kind (`true`, `nullptr`)
/// has a card only as the argument of a call.
auto literal_hover(const SelectionTree::Node& node,
                   const clang::Expr& literal,
                   const Request& request) -> std::optional<HoverInfo> {
    auto& context = request.unit.context();
    HoverInfo info;
    info.name = literal_text(request.unit, literal);
    /// The type alone: an alias is never what a literal is written with. An
    /// unevaluated string, as a `static_assert`'s message, has none.
    if(!literal.getType().isNull()) {
        info.type.emplace(display::type(context, literal.getType(), request.display).text);
    }
    if(const auto* string = llvm::dyn_cast<clang::StringLiteral>(&literal)) {
        info.kind = SymbolKind::String;
        info.length = string->getLength();
        info.size = (string->getLength() + 1) * string->getCharByteWidth() * 8;
    } else {
        info.kind = llvm::isa<clang::CharacterLiteral>(literal) ? SymbolKind::Character
                                                                : SymbolKind::Number;
        info.value = display::expr_value(context, &literal);
    }
    add_pass(node, info, request.display);
    if(!llvm::isa<clang::IntegerLiteral,
                  clang::FloatingLiteral,
                  clang::ImaginaryLiteral,
                  clang::FixedPointLiteral,
                  clang::CharacterLiteral,
                  clang::StringLiteral>(literal) &&
       !info.callee_arg_info) {
        return std::nullopt;
    }
    return info;
}

/// The standard defines __func__ as a "predefined variable".
auto predefined_hover(const clang::PredefinedExpr& expr,
                      clang::ASTContext& context,
                      const display::Options& options) -> HoverInfo {
    HoverInfo info;
    info.name = expr.getIdentKindName();
    info.kind = SymbolKind::Variable;
    info.documentation.brief = "Name of the current function (predefined variable)";
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

/// The card of `this`: the type it points to.
auto this_hover(const clang::CXXThisExpr& expr,
                clang::ASTContext& context,
                const display::Options& options) -> HoverInfo {
    auto pointee = expr.getType()->getPointeeType();
    /// The class as written: inside a partial specialization, `this` points
    /// to an injected class name whose arguments print as
    /// `type-parameter-0-0`.
    auto written = types::declared_type(pointee->getAsTagDecl());
    HoverInfo info;
    info.name = "this";
    info.definition =
        display::type(context,
                      context.getPointerType(
                          clang::QualType(written.getTypePtr(), pointee.getCVRQualifiers())),
                      options)
            .text;
    reformat(info.definition);
    return info;
}

/// The card of an expression: `this`, `__func__`, a literal, or one whose
/// value is known at compile time.
auto expr_hover(const SelectionTree::Node& node, const clang::Expr& expr, const Request& request)
    -> std::optional<HoverInfo> {
    auto& context = request.unit.context();
    /// `6.0i` is one token, the number in it a literal of its own.
    if(const auto* imaginary =
           node.parent ? node.parent->get<clang::ImaginaryLiteral>() : nullptr) {
        return literal_hover(*node.parent, *imaginary, request);
    }
    if(llvm::isa<clang::IntegerLiteral,
                 clang::FloatingLiteral,
                 clang::ImaginaryLiteral,
                 clang::FixedPointLiteral,
                 clang::CharacterLiteral,
                 clang::StringLiteral,
                 clang::CXXBoolLiteralExpr,
                 clang::CXXNullPtrLiteralExpr,
                 clang::CompoundLiteralExpr>(expr)) {
        return literal_hover(node, expr, request);
    }
    if(const auto* this_expr = llvm::dyn_cast<clang::CXXThisExpr>(&expr)) {
        return this_hover(*this_expr, context, request.display);
    }
    if(const auto* predefined = llvm::dyn_cast<clang::PredefinedExpr>(&expr)) {
        return predefined_hover(*predefined, context, request.display);
    }
    auto value = display::expr_value(context, &expr);
    if(!value) {
        return std::nullopt;
    }
    HoverInfo info;
    info.name = "expression";
    auto shown = request.display;
    shown.scope = &node.decl_context();
    shown.source = &expr;
    info.type = display::type(context, expr.getType(), shown);
    info.value = std::move(value);
    add_pass(node, info, request.display);
    return info;
}

/// The card of an attribute: as the source writes it, with clang's manual
/// entry. Clang's printer would fill in what the source leaves out
/// (`[[nodiscard("")]]`).
auto attr_hover(CompilationUnit& unit, const clang::Attr& attr) -> HoverInfo {
    HoverInfo info;
    info.kind = SymbolKind::Attribute;
    info.name = attr.getSpelling();
    if(attr.hasScope()) {
        info.name = (attr.getScopeName()->getName() + "::" + info.name).str();
    }
    info.definition = display::attribute(attr, unit.context());
    reformat(info.definition);
    info.attribute_documentation = clang::Attr::getDocumentation(attr.getKind()).str();
    return info;
}

/// The card of `auto` or `decltype`: the type it stands for, deduced from
/// `source`, with the documentation of what that names: an alias's own, not
/// that of the class behind it, which may be a library's internal one.
auto deduced_hover(clang::QualType type,
                   clang::tok::TokenKind keyword,
                   const clang::DeclContext* scope,
                   const clang::Expr* source,
                   const Request& request) -> HoverInfo {
    HoverInfo info;
    info.name = clang::tok::getTokenName(keyword);
    if(type->isUndeducedAutoType()) {
        info.definition = "/* not deduced */";
        return info;
    }

    auto options = request.display;
    options.scope = scope;
    options.source = source;
    /// What the keyword stands for carries the aliases of the declarations
    /// it was deduced from, as a placeholder's deduced type does.
    auto& context = request.unit.context();
    type = context.getAutoType(clang::DeducedKind::Deduced, type, clang::AutoTypeKeyword::Auto);
    auto shown = display::type(context, type, options);
    info.definition = shown.text;
    reformat(info.definition);
    add_alias(info, context, type, options);
    auto readable = options.readable->readable(type, scope, source);
    const clang::NamedDecl* named = nullptr;
    if(const auto* alias = readable->getAs<clang::TypedefType>()) {
        named = alias->getDecl();
    } else {
        named = readable->getAsTagDecl();
    }
    if(named) {
        info.documentation = documentation(request.unit, named, request.documentation);
    }
    return info;
}

/// The card of the directive argument at `range` of `content`, whose
/// target is `file`.
HoverInfo header_card(llvm::StringRef content, LocalSourceRange range, std::string file) {
    auto name = content.substr(range.begin, range.length()).trim();
    if(name.size() >= 2 && ((name.front() == '"' && name.back() == '"') ||
                            (name.front() == '<' && name.back() == '>'))) {
        name = name.drop_front().drop_back();
    }

    HoverInfo info;
    info.name = unspliced(name);
    info.kind = SymbolKind::Header;
    info.file = std::move(file);
    info.symbol_range = range;
    return info;
}

auto file_directive_hover(CompilationUnit& unit, std::uint32_t offset) -> std::optional<HoverInfo> {
    auto focus = unit.focus_file();
    auto content = focus.content();
    if(!focus.lexical().directive_at(content, offset)) {
        return std::nullopt;
    }
    for(auto& link: document_links(unit)) {
        if(link.range.contains(offset)) {
            return header_card(content, link.range, std::move(link.target));
        }
    }
    return std::nullopt;
}

/// The tokens the macro reference at `name` expands to, spelled and joined.
/// Written in another macro's arguments, it is expanded within that macro's
/// top-level expansion, whose tokens carry the chain of expansions they
/// went through.
auto expansion_preview(CompilationUnit& unit, clang::SourceLocation name) -> std::string {
    auto& sources = unit.context().getSourceManager();

    /// Whether the location lies in the reference's expansion.
    auto within = [&](clang::SourceLocation location) {
        while(location.isMacroID()) {
            const auto& expansion =
                sources.getSLocEntry(sources.getFileID(location)).getExpansion();
            if(!expansion.isMacroArgExpansion() && expansion.getExpansionLocStart() == name) {
                return true;
            }
            location = expansion.getExpansionLocStart();
        }
        return false;
    };

    /// The token as the reference's expansion produced it, invalid if it
    /// comes from elsewhere. An argument substituted outside that
    /// expansion is the token it was before: the macro whose arguments
    /// hold the reference may copy it for each use of its parameter.
    auto produced = [&](this auto& self, clang::SourceLocation token) -> clang::SourceLocation {
        for(auto location = token; location.isMacroID();) {
            const auto& expansion =
                sources.getSLocEntry(sources.getFileID(location)).getExpansion();
            if(expansion.isMacroArgExpansion()) {
                if(!within(expansion.getExpansionLocStart())) {
                    return self(sources.getImmediateSpellingLoc(location));
                }
            } else if(expansion.getExpansionLocStart() == name) {
                return token;
            }
            location = expansion.getExpansionLocStart();
        }
        return {};
    };

    std::string preview;
    constexpr std::size_t preview_limit = 1024;
    llvm::DenseSet<clang::SourceLocation> seen;
    auto& tokens = unit.focus_file().tokens();
    for(auto& expansion: tokens.expansions_overlapping(tokens.spelled_touching(name))) {
        for(const auto& token: expansion.expanded) {
            auto at = produced(token.location());
            if(at.isInvalid() || !seen.insert(at).second) {
                continue;
            }
            if(!preview.empty()) {
                preview += ' ';
            }
            /// Lex at the token's spelling site — getSpelling stays at the
            /// depth it is given, and only the spelling carries the written
            /// bytes.
            auto spelling = unit.token_spelling(sources.getSpellingLoc(token.location()));
            if(auto remaining = preview_limit - preview.size(); spelling.size() >= remaining) {
                preview.append(spelling, 0, remaining);
                preview += " ...";
                return preview;
            }
            preview += spelling;
        }
    }
    return preview;
}

/// The card of a macro: its `#define` as written, and its documentation.
auto macro_card(CompilationUnit& unit,
                const clang::MacroInfo& macro,
                const DocumentationOptions& options) -> HoverInfo {
    HoverInfo info;
    info.kind = SymbolKind::Macro;
    /// The definition text spans the name through the last body token.
    /// Its file can be synthetic (`-D` macros live in <command line>),
    /// which still has real `#define` lines as content.
    auto [fid, range] = *unit.written_range(
        clang::SourceRange(macro.getDefinitionLoc(), macro.getDefinitionEndLoc()));
    info.definition =
        "#define " + unit.file_content(fid).substr(range.begin, range.end - range.begin).str();
    info.documentation = documentation(unit, &macro, options);
    return info;
}

/// A macro name at the cursor, rendered from the preprocessor record —
/// macro semantics never reach the AST. The card shows the `#define` text,
/// plus a preview of the expanded tokens at expansion sites. A name a
/// directive tests before the file defines it, as a header guard's
/// `#ifndef` does, shows the definition that follows.
auto macro_hover(CompilationUnit& unit,
                 std::uint32_t offset,
                 const DocumentationOptions& documentation_options) -> std::optional<HoverInfo> {
    auto focus = unit.focus_file();
    if(auto directives = unit.directives().find(focus.file());
       directives != unit.directives().end()) {
        for(const auto& macro: directives->second.macros) {
            auto range = unit.written_range(macro.loc)->range;
            if(offset < range.begin || offset >= range.end) {
                continue;
            }

            auto info = macro_card(unit, *macro.macro, documentation_options);
            info.name = unit.token_spelling(macro.loc);
            info.symbol_range = range;
            if(macro.kind == MacroRef::Kind::Ref) {
                /// A conditional reference (#ifdef) expands nothing: no
                /// expanded token comes from it.
                info.expansion = expansion_preview(unit, macro.loc);
            }
            return info;
        }
    }

    auto content = focus.content();
    if(!focus.lexical().directive_at(content, offset)) {
        return std::nullopt;
    }
    auto& tokens = focus.tokens();
    for(const auto& token: tokens.spelled_touching(focus.location(offset))) {
        if(focus.kind(token) != clang::tok::identifier) {
            continue;
        }
        auto name = token.text(content);
        for(const auto* macro: unit.macro_definitions(name)) {
            auto defined = unit.seen_at(macro->getDefinitionLoc());
            if(!focus.contains(defined.file) || defined.offset <= offset) {
                continue;
            }
            auto info = macro_card(unit, *macro, documentation_options);
            info.name = name.str();
            info.symbol_range = token.range;
            info.defined_on_line = content.take_front(defined.offset).count('\n') + 1;
            return info;
        }
    }
    return std::nullopt;
}

/// What the hovered token refers to: the declarations, and the token naming
/// them, which the editor highlights.
struct Targets {
    llvm::SmallVector<const clang::NamedDecl*, 4> decls;
    const Token* token = nullptr;
};

/// The declarations the hovered token refers to, straight from the semantic
/// map: names anchored at the token are collected from its owner chain (the
/// emitting node is always an ancestor-or-self of the token's innermost
/// owner). Rightmost touched token wins, matching the selection bias, but a
/// separator — `:`, `=`, `&&`, `||`, `?` — leading to what it does not spell
/// (a range-for's `begin`, a copy's constructor) gives way to the token it
/// follows: `for(auto& x: xs)` hovered right after `x` is `x`.
auto decls_at(CompilationUnit& unit, llvm::ArrayRef<Token> touched) -> Targets {
    const Semantics& semantics = unit.get<Semantics>();
    auto spelled = semantics.spelled_tokens();
    auto focus = unit.focus_file();
    llvm::SmallVector<const clang::NamedDecl*, 4> decls;
    Targets separated;

    /// Whether the token is a later token the occurrence's name owns: any
    /// token of `~Foo` or `operator==` names the function, `Foo` the
    /// destructor rather than the class.
    auto inside = [&](const NameOccurrence& occurrence, clang::SourceLocation token) {
        if(occurrence.name_end.isInvalid() || !occurrence.owns_whole_name()) {
            return false;
        }
        auto at = unit.written_at(token);
        auto begin = unit.written_at(occurrence.location);
        auto end = unit.written_at(occurrence.name_end);
        return at.file == begin.file && at.file == end.file && begin.offset < at.offset &&
               at.offset <= end.offset;
    };

    for(const auto& token: views::reverse(touched)) {
        auto kind = focus.kind(token);
        if(should_ignore_token(kind)) {
            continue;
        }

        assert(spelled.data() <= &token && &token < spelled.end() && "a token of the focus file");
        auto index = static_cast<std::uint32_t>(&token - spelled.data());
        auto location = focus.location(token.range.begin);

        llvm::DenseSet<std::uint32_t> visited;
        llvm::SmallPtrSet<const clang::NamedDecl*, 4> seen;
        llvm::SmallVector<const clang::NamedDecl*, 4> named;
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

                for(auto& occurrence:
                    resolve_occurrences(semantics, n, &unit.get<types::TemplateResolver>())) {
                    if(unit.written_at(occurrence.location) == unit.written_at(location)) {
                        if(seen.insert(occurrence.decl).second) {
                            decls.push_back(occurrence.decl);
                        }
                    } else if(inside(occurrence, location) &&
                              !ranges::contains(named, occurrence.decl)) {
                        named.push_back(occurrence.decl);
                    }
                }
            }
        }

        if(!named.empty()) {
            return {.decls = std::move(named), .token = &token};
        }
        auto spells_operator = [](const clang::NamedDecl* decl) {
            return decl->getDeclName().getNameKind() == clang::DeclarationName::CXXOperatorName;
        };
        if(!decls.empty() && &token != &touched.front() &&
           ranges::contains(std::array{clang::tok::colon,
                                       clang::tok::equal,
                                       clang::tok::ampamp,
                                       clang::tok::pipepipe,
                                       clang::tok::question},
                            kind) &&
           ranges::none_of(decls, spells_operator)) {
            separated = {.decls = std::move(decls), .token = &token};
            decls.clear();
            continue;
        }
        if(!decls.empty()) {
            return {.decls = std::move(decls), .token = &token};
        }
    }
    return separated;
}

/// The declaration a card is about among those the hovered token names:
/// what a using-declaration brings in rather than the using-declaration,
/// unless it brings in an overload set and nothing tells which one: the
/// card of the using-declaration lists them all.
auto pick_decl(llvm::ArrayRef<const clang::NamedDecl*> candidates, const SelectionTree::Node& node)
    -> const clang::NamedDecl* {
    if(const auto* using_decl = node.get<clang::UsingDecl>();
       using_decl && brought_in(*using_decl).size() > 1) {
        return using_decl;
    }
    if(candidates.empty()) {
        return nullptr;
    }
    const auto* using_decl = ranges::find_if(candidates, [](const clang::NamedDecl* decl) {
        return llvm::isa<clang::UsingDecl>(decl);
    });
    if(using_decl == candidates.end()) {
        return candidates.front();
    }
    if(candidates.size() == 2) {
        return candidates[using_decl == candidates.begin() ? 1 : 0];
    }
    return *using_decl;
}

/// The specialization a class template name stands for where the code
/// deduces its arguments, `Box b(42)` or `Box(42)`: the type of the
/// declaration or the expression it is written in.
auto deduced_class(const SelectionTree::Node& node) -> const clang::CXXRecordDecl* {
    const auto* loc = node.get<clang::TypeLoc>();
    if(!loc || !loc->getAs<clang::DeducedTemplateSpecializationTypeLoc>()) {
        return nullptr;
    }
    for(const auto* outer = node.parent; outer; outer = outer->parent) {
        if(const auto* declarator = outer->get<clang::DeclaratorDecl>()) {
            return declarator->getType()->getAsCXXRecordDecl();
        }
        if(const auto* expr = outer->get<clang::Expr>()) {
            return expr->getType()->getAsCXXRecordDecl();
        }
        if(outer->get<clang::Decl>() || outer->get<clang::Stmt>()) {
            break;
        }
    }
    return nullptr;
}

/// Sizes (and padding) are shown in bytes if possible, otherwise in bits.
auto format_size(std::uint64_t bits) -> std::string {
    auto [value, unit] = bits % 8 == 0 ? std::pair(bits / 8, "byte") : std::pair(bits, "bit");
    return llvm::formatv("{0} {1}{2}", value, unit, value == 1 ? "" : "s").str();
}

/// An offset in bytes, and bits past them for a bitfield, so offsets of
/// different fields compare at a glance.
auto format_offset(std::uint64_t bits) -> std::string {
    auto offset = format_size(bits / 8 * 8);
    if(bits % 8 != 0) {
        offset += " and " + format_size(bits % 8);
    }
    return offset;
}

/// Plain reStructuredText with its inline markup taken out: backslash
/// escapes, *emphasis*, **strong** and the trailing underscore of a
/// reference_.
auto rst_plain(llvm::StringRef text) -> std::string {
    constexpr llvm::StringLiteral opening = " \t([{<'\"-/:";
    constexpr llvm::StringLiteral closing = " \t)]}>'\"-/:.,;!?";

    std::string plain;
    for(std::size_t i = 0; i < text.size(); i += 1) {
        char c = text[i];
        if(c == '\\' && i + 1 < text.size()) {
            i += 1;
            if(!llvm::isSpace(text[i])) {
                plain += text[i];
            }
            continue;
        }

        if(c == '*' && (i == 0 || opening.contains(text[i - 1]))) {
            auto marker = text.substr(i).starts_with("**") ? llvm::StringRef("**") : "*";
            auto inner = i + marker.size();
            auto end = text.find(marker, inner);
            if(inner < text.size() && !llvm::isSpace(text[inner]) && end != llvm::StringRef::npos &&
               !llvm::isSpace(text[end - 1]) &&
               (end + marker.size() == text.size() ||
                closing.contains(text[end + marker.size()]))) {
                plain += rst_plain(text.slice(inner, end));
                i = end + marker.size() - 1;
                continue;
            }
        }

        if(c == '_' && i > 0 && llvm::isAlnum(text[i - 1]) &&
           (i + 1 == text.size() || closing.contains(text[i + 1]))) {
            continue;
        }
        plain += c;
    }
    return plain;
}

/// The text of inline reStructuredText: ``literals`` and interpreted text
/// as code, links and cross references (`text <target>`_, :ref:`text
/// <target>`) as their text.
void parse_rst_inline(llvm::StringRef text, markup::Paragraph& out) {
    while(!text.empty()) {
        auto tick = text.find('`');
        if(tick == llvm::StringRef::npos) {
            out.append_text(rst_plain(text));
            return;
        }

        if(text.substr(tick).starts_with("``")) {
            auto end = text.find("``", tick + 2);
            if(end == llvm::StringRef::npos) {
                out.append_text(rst_plain(text));
                return;
            }
            out.append_text(rst_plain(text.substr(0, tick)));
            out.append_code(text.slice(tick + 2, end));
            text = text.substr(end + 2);
            continue;
        }

        auto end = text.find('`', tick + 1);
        if(end == llvm::StringRef::npos) {
            out.append_text(rst_plain(text));
            return;
        }

        /// A role names its kind before the backtick, as in :ref:`...`.
        auto before = text.substr(0, tick);
        bool role = false;
        if(before.ends_with(":")) {
            auto start = before.drop_back().rfind(':');
            if(start != llvm::StringRef::npos &&
               ranges::all_of(before.slice(start + 1, before.size() - 1),
                              [](char c) { return llvm::isAlpha(c) || c == '-'; })) {
                before = before.substr(0, start);
                role = true;
            }
        }
        out.append_text(rst_plain(before));

        auto content = text.slice(tick + 1, end);
        text = text.substr(end + 1);
        bool link = text.starts_with("_");
        while(text.consume_front("_")) {}

        if(!role && !link) {
            /// Interpreted text without a role: the manual writes flags and
            /// names this way.
            out.append_code(content);
            continue;
        }
        if(content.ends_with(">")) {
            if(auto target = content.rfind('<'); target != llvm::StringRef::npos) {
                content = content.substr(0, target).rtrim();
            }
        }
        out.append_text(content);
    }
}

/// Renders clang's attribute documentation, which is reStructuredText
/// written for its manual: paragraphs, list items, code blocks and the
/// note and warning directives.
void parse_rst(llvm::StringRef input, markup::Document& output) {
    llvm::SmallVector<llvm::StringRef> lines;
    input.split(lines, '\n');

    auto indent_of = [](llvm::StringRef line) {
        return line.size() - line.ltrim(' ').size();
    };

    /// The lines after `from` indented deeper than `indent`, blank lines
    /// among them, as a block without its common indentation.
    auto take_block = [&](std::size_t& from, std::size_t indent) {
        std::size_t end = from;
        std::size_t common = std::string::npos;
        for(std::size_t i = from; i < lines.size(); i += 1) {
            if(lines[i].trim().empty()) {
                continue;
            }
            if(indent_of(lines[i]) <= indent) {
                break;
            }
            common = std::min(common, indent_of(lines[i]));
            end = i + 1;
        }

        std::string block;
        for(std::size_t i = from; i < end; i += 1) {
            if(!block.empty() || !lines[i].trim().empty()) {
                block += lines[i].trim().empty() ? "" : lines[i].substr(common).rtrim();
                block += '\n';
            }
        }
        from = end;
        return llvm::StringRef(block).rtrim().str();
    };

    std::string paragraph;
    auto flush = [&] {
        if(auto text = llvm::StringRef(paragraph).trim(); !text.empty()) {
            parse_rst_inline(text, output.add_paragraph());
        }
        paragraph.clear();
    };

    /// The number of the last `#.` item of the list going on, 0 outside one.
    std::size_t numbered = 0;
    for(std::size_t i = 0; i < lines.size();) {
        auto text = lines[i].trim();
        if(text.empty()) {
            flush();
            i += 1;
            continue;
        }

        if(text.consume_front(".. ")) {
            flush();
            auto indent = indent_of(lines[i]);
            auto [directive, argument] = text.split("::");
            argument = argument.trim();
            i += 1;
            auto body = take_block(i, indent);
            if(directive == "code-block" || directive == "code") {
                output.add_code_block(std::move(body), argument == "c++" ? "cpp" : argument.str());
            } else if(directive.equals_insensitive("note") ||
                      directive.equals_insensitive("warning")) {
                paragraph = (directive.equals_insensitive("note") ? "Note: " : "Warning: ") +
                            argument.str() + " " + body;
                flush();
            }
            continue;
        }

        bool auto_numbered = text.starts_with("#. ");
        bool item = auto_numbered || text.starts_with("- ") || text.starts_with("* ") ||
                    (llvm::isDigit(text[0]) && text.drop_while(llvm::isDigit).starts_with(". "));
        if(item || paragraph.empty()) {
            flush();
            if(auto_numbered) {
                numbered += 1;
                paragraph = std::to_string(numbered);
                text = text.drop_front(1);
            } else if(indent_of(lines[i]) == 0) {
                numbered = 0;
            }
        }
        paragraph += text;
        paragraph += ' ';
        i += 1;
    }
    flush();
}

}  // namespace

/// Exhaustive so a new kind fails to compile rather than silently lose its
/// noun.
auto kind_noun(SymbolKind kind) -> llvm::StringRef {
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
        /// Only an index card has it: whether the operator is a member is
        /// not stored.
        case SymbolKind::Operator:
        case SymbolKind::Function: return "function";
        case SymbolKind::Method: return "method";
        case SymbolKind::Variable: return "variable";
        case SymbolKind::Parameter: return "parameter";
        case SymbolKind::Label: return "label";
        case SymbolKind::Macro: return "macro";
        case SymbolKind::Header: return "header";
        case SymbolKind::Attribute: return "attribute";
        case SymbolKind::Number:
        case SymbolKind::Character:
        case SymbolKind::String: return "literal";
        case SymbolKind::Comment:
        case SymbolKind::Keyword:
        case SymbolKind::Directive:
        case SymbolKind::Invalid: return "";
    }
    std::unreachable();
}

auto to_protocol(const HoverInfo& info,
                 protocol::MarkupKind kind,
                 const PositionMap& map,
                 PositionEncoding encoding) -> protocol::Hover {
    auto document = info.present();

    protocol::Hover result{
        .contents =
            protocol::MarkupContent{
                                    .kind = kind,
                                    .value = kind == protocol::MarkupKind::Markdown ? document.as_markdown()
                                                                : document.as_plain_text(),
                                    },
    };

    if(info.symbol_range) {
        result.range = map.range(*info.symbol_range, encoding);
    }

    return result;
}

markup::Document HoverInfo::present() const {
    markup::Document output;

    /// One line names the entity and where it is: `function translate ·
    /// namespace geo`.
    auto& title = output.add_paragraph();
    if(auto word = kind_noun(kind); !word.empty()) {
        title.append_strong(access.empty() ? word.str() : access + " " + word.str());
        title.append_space().append_code(name);
    } else {
        title.append_strong(name);
    }
    if(!scope_kind.empty()) {
        title.append_text(" · " + scope_kind + " ");
        if(!scope.empty()) {
            title.append_code(scope);
        }
    }
    output.add_spacing();

    add_summary(output, documentation, deprecation);

    if(!definition.empty()) {
        output.add_code_block(definition);
    }
    if(!target_definition.empty()) {
        output.add_code_block(target_definition);
    }
    if(!expansion.empty()) {
        output.add_paragraph().append_text("Expands to:");
        output.add_code_block(expansion);
    }
    if(!file.empty()) {
        output.add_paragraph().append_code(file);
    }

    if(type) {
        auto& line = output.add_paragraph().append_text("Type: ").append_code(type->text);
        if(type->aka) {
            line.append_text(" (aka ").append_code(*type->aka).append_text(")");
        }
    }
    for(const auto& alias: aliases) {
        output.add_paragraph().append_code(alias.text).append_text(" is ").append_code(*alias.aka);
    }
    if(value) {
        output.add_paragraph().append_text("Value: ").append_code(*value);
    }
    if(length) {
        output.add_paragraph().append_text(llvm::formatv("Length: {0}", *length).str());
    }
    if(defined_on_line) {
        output.add_paragraph().append_text(
            llvm::formatv("Not defined yet here: defined on line {0}", *defined_on_line).str());
    }
    if(!overrides.empty()) {
        auto& line = output.add_paragraph().append_text("Overrides: ");
        for(auto [index, overridden]: views::enumerate(overrides)) {
            if(index != 0) {
                line.append_text(", ");
            }
            line.append_code(overridden);
        }
    }
    if(offset) {
        output.add_paragraph().append_text("Offset: " + format_offset(*offset));
    }
    if(size) {
        auto& line = output.add_paragraph().append_text("Size: " + format_size(*size));
        if(padding && *padding != 0) {
            /// A field's padding follows it; a class's is spread inside it.
            line.append_text(
                llvm::formatv(kind == SymbolKind::Field ? " (+{0} padding)" : " ({0} padding)",
                              format_size(*padding))
                    .str());
        }
        if(align) {
            line.append_text(", alignment " + format_size(*align));
        }
    }
    if(callee_arg_info) {
        assert(pass_mode);
        auto& line = output.add_paragraph().append_text("Passed ");
        if(callee_arg_info->name) {
            line.append_text("as ").append_code(*callee_arg_info->name);
        }
        switch(*pass_mode) {
            case PassMode::Value: line.append_text(" by value"); break;
            case PassMode::Ref: line.append_text(" by reference"); break;
            case PassMode::ConstRef: line.append_text(" by const reference"); break;
            case PassMode::RValueRef: line.append_text(" by rvalue reference"); break;
        }
        if(converted_to) {
            line.append_text(", converted to ").append_code(*converted_to);
        }
    }

    if(!documentation.template_params.empty() || !documentation.params.empty() ||
       !documentation.returns.empty() || !documentation.retvals.empty() ||
       !documentation.throws.empty()) {
        output.add_spacing();
    }
    add_parameters(output, "Template parameters: ", documentation.template_params);
    add_parameters(output, "Parameters: ", documentation.params);
    if(!documentation.returns.empty()) {
        output.add_paragraph().append_text("Returns: ").append_markdown(documentation.returns);
    }
    add_entries(output, "Return values: ", documentation.retvals);
    add_entries(output, "Throws: ", documentation.throws);

    if(!attribute_documentation.empty()) {
        output.add_ruler();
        parse_rst(attribute_documentation, output);
    }
    if(!documentation.details.empty() || !documentation.see.empty()) {
        output.add_ruler();
        add_details(output, documentation);
    }
    if(!attribute_macros.empty()) {
        output.add_spacing();
    }
    for(const auto& macro: attribute_macros) {
        output.add_paragraph().append_code(macro.use).append_text(" is ").append_code(
            macro.attributes);
    }
    return output;
}

auto hover_info(CompilationUnit& unit,
                std::uint32_t offset,
                const HoverOptions& options,
                const DocumentationOptions& documentation) -> std::optional<HoverInfo> {
    /// The hover is over an include or embed directive.
    if(auto info = file_directive_hover(unit, offset)) {
        return info;
    }

    if(auto info = macro_hover(unit, offset, documentation)) {
        reformat(info->expansion);
        return info;
    }

    auto& context = unit.context();
    display::Options display_options = {
        .terse = true,
        .polish_for_declaration = true,
        .constants_as_written = true,
        .suppress_ctor_template_args = true,
        .resolve_decltype = true,
        .show_aka = options.show_aka,
        .max_members = options.max_members,
    };
    display::ReadableTypes readable(context, display_options);
    display_options.readable = &readable;
    Request request{
        .unit = unit,
        .documentation = documentation,
        .display = display_options,
    };

    auto focus = unit.focus_file();
    auto tokens = focus.tokens().spelled_touching(focus.location(offset));

    /// Early exit if there were no tokens around the cursor.
    if(tokens.empty()) {
        return std::nullopt;
    }

    /// To be used as a backup for highlighting the selected token, we use back
    /// as it aligns better with biases elsewhere (editors tend to send the
    /// position for the left of the hovered token).
    LocalSourceRange highlight_range = tokens.back().range;
    std::optional<HoverInfo> info;

    /// Deduced type only works on auto/decltype keywords.
    for(const auto& token: tokens) {
        auto kind = focus.kind(token);
        if(kind == clang::tok::identifier) {
            /// Prefer the identifier token as a fallback highlighting range.
            highlight_range = token.range;
        } else if(kind == clang::tok::kw_auto || kind == clang::tok::kw_decltype) {
            auto tree = SelectionTree::create_right(unit, token.range);
            if(const auto* node = tree.common_ancestor()) {
                auto deduced = types::deduced_type(node->data, node->owning_decl());
                /// A placeholder of an expression, `new auto(1)` or `auto(x)`,
                /// stands for the type the expression creates.
                for(const auto* outer = node; deduced.isNull() && outer; outer = outer->parent) {
                    if(const auto* allocation = outer->get<clang::CXXNewExpr>()) {
                        deduced = allocation->getAllocatedType();
                    } else if(const auto* cast = outer->get<clang::CXXFunctionalCastExpr>()) {
                        deduced = cast->getType();
                    } else if(outer->get<clang::Decl>() || outer->get<clang::Stmt>()) {
                        break;
                    }
                }
                if(const auto* var = llvm::dyn_cast_if_present<clang::VarDecl>(node->owning_decl());
                   var && !deduced.isNull() && deduced->isUndeducedAutoType()) {
                    if(auto resolved = resolved_auto(var, unit); !resolved.isNull()) {
                        deduced = resolved;
                    }
                }
                if(!deduced.isNull()) {
                    const auto* written = node->data.get<clang::TypeLoc>();
                    const auto* operand =
                        written ? llvm::dyn_cast<clang::DecltypeType>(written->getTypePtr())
                                : nullptr;
                    const auto* var = llvm::dyn_cast<clang::VarDecl>(node->owning_decl());
                    const clang::Expr* source = operand ? operand->getUnderlyingExpr()
                                                : var   ? var->getInit()
                                                        : nullptr;
                    info = deduced_hover(deduced,
                                         kind,
                                         node->owning_decl()->getDeclContext(),
                                         source,
                                         request);
                    highlight_range = token.range;
                    break;
                }
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
            const auto* decl = pick_decl(targets.decls, *node);
            if(const auto* deduced = deduced_class(*node); deduced && decl) {
                decl = deduced;
            }
            if(decl) {
                auto through = display_options;
                through.specialization = accessed_specialization(*node, unit);
                info =
                    decl_hover(decl,
                               {.unit = unit, .documentation = documentation, .display = through});
                if(options.layout) {
                    add_layout(*templated(decl), *info);
                }
                if(!info->value) {
                    info->value = own_value(*node, *decl, context);
                }
                add_pass(*node, *info, display_options);
                if(targets.token) {
                    highlight_range = targets.token->range;
                }
            } else if(const auto* expr = node->get<clang::Expr>()) {
                info = expr_hover(*node, *expr, request);
            } else if(const auto* attr = node->get<clang::Attr>()) {
                info = attr_hover(unit, *attr);
            }
        }
    }

    if(!info) {
        return std::nullopt;
    }

    info->symbol_range = highlight_range;
    return info;
}

auto hover(CompilationUnit& unit,
           std::uint32_t offset,
           const HoverOptions& options,
           const DocumentationOptions& documentation,
           protocol::MarkupKind kind,
           PositionEncoding encoding) -> std::optional<protocol::Hover> {
    auto info = hover_info(unit, offset, options, documentation);
    if(!info) {
        return std::nullopt;
    }

    return to_protocol(*info, kind, unit.focus_file().positions(), encoding);
}

/// The text of a definition before the body its extent ends with, or
/// nullopt when the extent ends otherwise (`= default`, an initializer).
static std::optional<llvm::StringRef> before_body(const std::string& text) {
    // FIXME: a function-try-block's last brace group is its last handler:
    // the card keeps the body and the `catch (...)` heads.
    auto braces = code_braces(lexical_scan(text, dialect_of()));
    auto written = llvm::StringRef(text).rtrim();
    auto body = last_brace_group(braces, {0, static_cast<std::uint32_t>(written.size())});
    if(!body || body->end != written.size()) {
        return std::nullopt;
    }
    return written.take_front(body->begin).rtrim();
}

auto hover_info(const index::SymbolInfo& info,
                llvm::ArrayRef<index::SymbolInfo> scopes,
                llvm::StringRef definition_text,
                Documentation documentation) -> HoverInfo {
    HoverInfo hover;
    hover.name = info.display_name();
    hover.kind = info.kind;
    /// The parent chain skips anonymous namespaces, which the card names as
    /// the AST card does: a namespace path as `(anonymous)`, the scope of a
    /// class or function as clang prints it.
    auto anonymous = [](const index::SymbolInfo& symbol) {
        return index::has_flag(symbol.flags, index::SymbolFlags::AnonymousNamespace);
    };
    if(anonymous(info)) {
        hover.scope_kind = "namespace";
    } else if(!scopes.empty()) {
        hover.scope_kind = kind_noun(scopes.back().kind).str();
    }
    llvm::StringRef unnamed =
        hover.scope_kind == "namespace" ? "(anonymous)" : "(anonymous namespace)";
    llvm::SmallVector<std::string> names;
    for(const auto& scope: scopes) {
        if(anonymous(scope)) {
            names.push_back(unnamed.str());
        }
        names.push_back(scope.display_name());
    }
    if(anonymous(info)) {
        names.push_back(unnamed.str());
    }
    hover.scope = llvm::join(names, "::");
    hover.documentation = std::move(documentation);
    /// The index keeps that a symbol is deprecated, not the message.
    if(index::has_flag(info.flags, index::SymbolFlags::Deprecated)) {
        hover.deprecation.emplace();
    }
    if(definition_text.empty()) {
        return hover;
    }

    // The stored extent is the whole written definition; the AST card
    // prints a declaration — a function without its body, a namespace with
    // an empty one, a macro with its `#define`. A type's members are left
    // out: the AST card's member summary needs the AST.
    hover.definition = definition_text.str();
    switch(info.kind) {
        case SymbolKind::Function:
        case SymbolKind::Method:
        case SymbolKind::Operator: {
            if(auto head = before_body(hover.definition)) {
                hover.definition = head->str();
            }
            break;
        }
        case SymbolKind::Namespace: {
            if(auto head = before_body(hover.definition)) {
                hover.definition = head->str() + " {}";
            }
            break;
        }
        case SymbolKind::Class:
        case SymbolKind::Struct:
        case SymbolKind::Union:
        case SymbolKind::Enum: {
            if(auto head = before_body(hover.definition)) {
                hover.definition = head->str() + " { ... }";
            }
            break;
        }
        case SymbolKind::Macro: {
            hover.definition.insert(0, "#define ");
            break;
        }
        default: break;
    }
    if(index::has_flag(info.flags, index::SymbolFlags::Exported)) {
        hover.definition.insert(0, "export ");
    }
    return hover;
}

auto hover_info(const index::PeerQueries& queries,
                const index::IndexQuery::Cursor& cursor,
                const DocumentationOptions& documentation) -> std::optional<HoverInfo> {
    auto& query = queries.own;
    auto symbol = cursor.symbols.front();
    auto info = query.symbol_info(symbol, cursor.site.file);
    if(!info) {
        return std::nullopt;
    }
    if(info->kind == SymbolKind::Module) {
        return module_hover(queries, cursor, *info);
    }
    std::string definition;
    auto text = index::declared_text(query, symbol, cursor.site.file, RelationKind::Definition);
    if(!text) {
        text = index::declared_text(query, symbol, cursor.site.file, RelationKind::Declaration);
    }
    if(text) {
        definition = std::move(text->text);
    }
    auto hover = hover_info(*info,
                            query.container_chain(symbol, cursor.site.file),
                            definition,
                            feature::documentation(query, symbol, cursor.site.file, documentation));
    hover.symbol_range = cursor.site.range;
    return hover;
}

auto module_hover(const index::PeerQueries& queries,
                  const index::IndexQuery::Cursor& cursor,
                  const index::SymbolInfo& module) -> HoverInfo {
    HoverInfo hover;
    hover.name = module.name;
    hover.kind = SymbolKind::Module;
    auto units =
        queries.gather(module.hash,
                       cursor.site.file,
                       [&](const index::IndexQuery& from, index::SymbolHash named) {
                           return from.sites(named, cursor.site.file, RelationKind::Definition);
                       });
    if(!units.empty()) {
        hover.file = queries.own.file_table().display_identity(units.front().path);
    }
    hover.symbol_range = cursor.site.range;
    return hover;
}

auto directive_hover(llvm::StringRef text,
                     const PositionMap& map,
                     FileTable& files,
                     const index::DocumentLink& link,
                     protocol::MarkupKind kind) -> std::optional<protocol::Hover> {
    if(link.range.end > text.size()) {
        return std::nullopt;
    }
    auto info = header_card(text, link.range, files.display_identity(link.target));

    auto hover = to_protocol(info, kind, map, PositionEncoding::UTF16);
    if(!hover.range) {
        return std::nullopt;
    }
    return hover;
}

}  // namespace clice::feature
