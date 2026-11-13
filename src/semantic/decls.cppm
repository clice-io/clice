module clice:semantic.decls;

import :support.prelude;

/// Declaration-centric AST helpers: template predicates, instantiation
/// navigation and call/parameter analysis. Type queries live in types.h,
/// rendering in display.h.
namespace clice::decls {

/// Check whether the decl is a template. Note that for partial
/// specializations, we consider it as a template while clang does not.
bool is_templated(const clang::Decl* decl);

/// The parameter list a template or a partial specialization declares;
/// none for any other declaration.
auto own_template_parameters(const clang::Decl* decl) -> const clang::TemplateParameterList*;

/// The template parameter a substitution names by its associated
/// declaration and index, as clang's `getReplacedParameter()` and
/// `getParameterPack()` find it, or null when that declaration's list has
/// no parameter of the kind at the index. clang trusts the index, but a
/// specialization merged across modules can report its primary template
/// while the index came from the partial specialization it was
/// instantiated from (`function_ref<Ret(Params...)>`'s `Params`).
auto replaced_parameter(const clang::SubstTemplateTypeParmType& type)
    -> const clang::TemplateTypeParmDecl*;
auto replaced_parameter(const clang::SubstNonTypeTemplateParmPackExpr& expr)
    -> const clang::NonTypeTemplateParmDecl*;

/// The parameter lists an out-of-line definition repeats for the class
/// templates enclosing it.
auto outer_template_parameters(const clang::Decl* decl)
    -> llvm::ArrayRef<clang::TemplateParameterList*>;

/// The source range the decl is written over: a template's from its
/// `template` keyword and the attributes leading it, which the templated
/// decl's own range leaves out; an instantiation's, its pattern's. A bound
/// clang leaves unlocated is the decl's name, the end of a namespace left
/// open the end of its file.
auto written_range(const clang::Decl* decl) -> clang::SourceRange;

/// Whether a `#pragma clang attribute` region applies the attribute at
/// `location`, as a directive or as the `_Pragma` operator in a macro: the
/// attribute is written on the pragma's line, not on the declaration.
bool applied_by_pragma(const clang::ASTContext& context, clang::SourceLocation location);

/// Check whether the decl names an entity its module exports: declared at
/// namespace scope by an `export` declaration, or a namespace holding one.
/// Members of an exported class are not exported entities of their own.
bool is_exported(const clang::Decl* decl);

/// The outermost namespace the decl is declared in, the library it belongs
/// to (`std`, `fmt`); null for one at the global scope.
const clang::NamespaceDecl* outermost_namespace(const clang::Decl* decl);

/// Check whether the decl is an implicit template instantiation.
bool is_implicit_instantiation(const clang::NamedDecl* decl);

/// Whether a declaration of the class or variable template specialization
/// `spec` explicitly specializes it. An explicit specialization named
/// before it is declared keeps that first, undeclared node as its
/// canonical declaration.
template <typename Spec>
bool explicitly_specialized(const Spec* spec) {
    for(auto* redecl: spec->redecls()) {
        if(llvm::cast<Spec>(redecl)->getSpecializationKind() == clang::TSK_ExplicitSpecialization) {
            return true;
        }
    }
    return false;
}

/// Check whether the decl heads a template-instantiation subtree: an
/// implicit instantiation, or the specialization an explicit instantiation
/// directive creates. Everything instantiated reuses the pattern's source
/// locations; the directive itself is a written decl of its own
/// (ExplicitInstantiationDecl).
bool is_instantiation(const clang::Decl* decl);

/// Whether the declaration is the standard library's own: written in a
/// system header, inside namespace `std` or a namespace whose name the
/// standard reserves for the implementation (libstdc++'s `__gnu_cxx`).
bool in_standard_library(const clang::Decl* decl);

/// Whether `function` is one of the standard library's cast-like forms —
/// `addressof`, `as_const`, `forward`, `forward_like`, `move`,
/// `move_if_noexcept` — clang knows as library builtins.
bool std_cast_form(const clang::FunctionDecl* function);

/// Whether `decl` is a function of the standard library whose meaning the
/// standard fixes, the same in every unit declaring it: a cast-like form
/// (std_cast_form) or `declval`.
bool fixed_std_utility(const clang::NamedDecl* decl);

/// Return the decl where it is instantiated from. It could be a template
/// decl or a member of a class template. If the decl is a full
/// specialization, return itself.
auto instantiated_from(const clang::NamedDecl* decl) -> const clang::NamedDecl*;

/// The canonical decl, folding instantiations back to the canonical decl
/// of their pattern, and an Objective-C @implementation back to the
/// @interface (or the category) it defines.
auto normalize(const clang::NamedDecl* decl) -> const clang::NamedDecl*;

/// The @interface or the category an Objective-C @implementation defines;
/// any other decl itself.
auto objc_defined(const clang::NamedDecl* decl) -> const clang::NamedDecl*;

/// The Objective-C class whose members a context declares: an @interface,
/// its @implementation, one of its extensions or categories; null for a
/// protocol and for any other context.
auto objc_class(const clang::DeclContext* context) -> const clang::ObjCInterfaceDecl*;

/// The method of `method`'s selector a receiver of the Objective-C class
/// `receiver` (null: any) runs as far as that class tells, as the code
/// writes it. Clang names the declaration its lookup found, which for a
/// method the class adopts from a protocol, or declares in its
/// @implementation alone, is the protocol's or a superclass's: the class,
/// or a superclass below the one declaring it, that has a method of the
/// selector names it instead; and an accessor clang declares for a
/// property is the method an @implementation writes, if this unit sees it.
auto objc_dispatched(const clang::ObjCInterfaceDecl* receiver,
                     const clang::ObjCMethodDecl& method,
                     bool instance) -> const clang::ObjCMethodDecl*;

/// Where an Objective-C message is named: at its first selector piece
/// with a name, where its first reference lies, else where its selector
/// starts.
auto objc_message_name(const clang::ObjCMessageExpr& message) -> clang::SourceLocation;

/// Whether an Objective-C message goes to `id` or `Class`, which may reach
/// any method of its selector. `self` in a class method is a `Class` clang
/// knows to be its class.
auto objc_untyped(const clang::ObjCMessageExpr& message) -> bool;

/// The method an Objective-C message runs as far as its receiver's static
/// class tells (objc_dispatched).
auto objc_message_target(const clang::ObjCMessageExpr& message) -> const clang::ObjCMethodDecl*;

/// The property an Objective-C instance variable stores when it
/// synthesizes the variable with no @synthesize written; null otherwise.
auto objc_unwritten_property(const clang::ObjCIvarDecl& ivar) -> const clang::ObjCPropertyDecl*;

/// Where `@compatibility_alias Quill Pen;` writes the alias and the class:
/// clang locates the declaration at its `@` alone. Invalid when a macro
/// writes them.
auto objc_alias_names(const clang::ObjCCompatibleAliasDecl& alias)
    -> std::pair<clang::SourceLocation, clang::SourceLocation>;

/// The `(` of an Objective-C class extension, `@interface Shape ()`, which
/// clang does not locate; invalid when it does not follow the class's name
/// (`@interface Box<T> ()`).
auto objc_extension_paren(const clang::ObjCCategoryDecl& extension) -> clang::SourceLocation;

/// A method of `selector` an Objective-C class answers: implemented or
/// declared by it or a superclass, or by a protocol it adopts, an instance
/// method before a class method; null for none.
auto objc_selector_method(const clang::ObjCInterfaceDecl& objc_class, clang::Selector selector)
    -> const clang::ObjCMethodDecl*;

/// Where `@selector(setX:y:)` writes each piece of its selector, which
/// clang does not locate: invalid for a piece with no name.
auto objc_selector_pieces(const clang::ObjCSelectorExpr& expr, const clang::ASTContext& context)
    -> llvm::SmallVector<clang::SourceLocation, 4>;

/// What a name at `location` naming the Objective-C class `objc_class`
/// writes: the class, or a `@compatibility_alias` of it.
auto objc_class_name(const clang::ObjCInterfaceDecl* objc_class, clang::SourceLocation location)
    -> const clang::NamedDecl*;

/// The nearest superclass method an Objective-C method overrides, null for
/// none; getOverriddenMethods leaves out one only an @implementation or a
/// category declares.
auto objc_overridden(const clang::ObjCMethodDecl& method) -> const clang::ObjCMethodDecl*;

/// The constructor a construction runs, as the source names it: an
/// inherited constructor is an implicit declaration standing in for the
/// base constructor `using Base::Base` names.
auto constructor_of(const clang::CXXConstructExpr* construct) -> const clang::CXXConstructorDecl*;

/// If exactly one non-explicit specialization of the templated decl
/// exists, return it, nullptr otherwise.
auto only_instantiation(clang::NamedDecl* templated_decl) -> clang::NamedDecl*;

/// Same for a parameter of a function template: the corresponding
/// parameter in the template's only instantiation.
auto only_instantiation(clang::ParmVarDecl* param) -> clang::ParmVarDecl*;

/// The declaration `function` was instantiated from. A member template of a
/// class template's specialization keeps no link of its own: its template
/// does.
auto function_pattern(const clang::FunctionDecl& function) -> const clang::FunctionDecl*;

/// The parameter `index` of each declaration of `function` that names it,
/// or of the one it was instantiated from: a name left out of one
/// declaration is written in another (a definition, a forward declaration
/// such as libstdc++'s `std::max`), and a definition may name differently
/// what the documented declaration names.
auto named_parameters(const clang::FunctionDecl& function, unsigned index)
    -> llvm::SmallVector<const clang::ParmVarDecl*, 2>;

/// The same of a function's parameter; one of a function pointer's type,
/// no parameter of the function around it, names only itself.
auto named_parameters(const clang::ParmVarDecl& param)
    -> llvm::SmallVector<const clang::ParmVarDecl*, 2>;

/// Returns the template parameter pack type that this parameter was
/// expanded from (if in the Args... or Args&... or Args&&... form), if
/// this is the case, nullptr otherwise.
auto underlying_pack_type(const clang::ParmVarDecl* param) -> const clang::TemplateTypeParmType*;

/// Returns the parameters that are forwarded from the template
/// parameters. For example, `template <typename... Args> void foo(Args...
/// args)` will return the `args` parameters.
auto resolve_forwarding_params(const clang::FunctionDecl* decl, unsigned max_depth = 10)
    -> llvm::SmallVector<const clang::ParmVarDecl*>;

/// Whether an argument for `param` is used through a mutable lvalue
/// reference, given `forwarded`, the parameter resolve_forwarding_params
/// resolves it to (`param` itself outside a pack). The argument's
/// eventual use is what counts, approximated as follows:
/// 1. the value category can only change from rvalue to lvalue during
///    forwarding, so `param` and `forwarded` both being lvalue references
///    means the category was preserved;
/// 2. an argument copied or cast somewhere along the chain can only reach
///    an rvalue or const lvalue reference parameter, so a mutable lvalue
///    reference at the end means it never was.
/// A pack parameter left unresolved says nothing about the eventual use.
bool binds_mutable_reference(const clang::ParmVarDecl* param, const clang::ParmVarDecl* forwarded);

/// An argument of a call and the parameter it binds to, with the one
/// resolve_forwarding_params resolves that to (the same outside a pack).
struct ParameterBinding {
    const clang::Expr* argument;
    const clang::ParmVarDecl* param;
    const clang::ParmVarDecl* forwarded;

    /// Whether the callee may modify the argument through a mutable lvalue
    /// reference (binds_mutable_reference), a forwarding reference, which
    /// binds anything, excluded.
    bool mutable_reference() const;
};

/// The arguments of a call or construction with the parameters they bind
/// to, in order. The object a call is made on and the left operand of an
/// overloaded operator are left out, and so are the arguments of a call
/// through no declared function and those past the last parameter.
auto parameter_bindings(const clang::Stmt* call) -> llvm::SmallVector<ParameterBinding>;

/// The template arguments a callee names explicitly, `<Widget>` in
/// `make_unique<Widget>`, dependent or not.
auto explicit_template_arguments(const clang::Expr* callee)
    -> llvm::ArrayRef<clang::TemplateArgumentLoc>;

}  // namespace clice::decls
