#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclTemplate.h"

namespace clice {

/// Sema-free structural unification of template argument lists, replacing
/// `Sema::DeduceTemplateArguments` and `Sema::getMoreSpecializedPartialSpecialization`
/// for pseudo-instantiation.
///
/// Differences from clang's deduction, by design:
///   - Works on sugared types: a parameter binds the argument *as written*
///     (e.g. `T = std::string`, not `T = std::basic_string<char, ...>`), so no
///     separate resugar pass is needed downstream.
///   - Binds template parameters to dependent arguments (`T = U`), which real
///     deduction never faces but pseudo-instantiation relies on.
///   - Skips conformance corners irrelevant to lookup (reference collapsing
///     adjustments, array bound promotion, constraint checks). Failure means
///     "this pattern doesn't match", which degrades to an unresolved name.
class TypeUnifier {
public:
    using TemplateArguments = llvm::ArrayRef<clang::TemplateArgument>;

    explicit TypeUnifier(clang::ASTContext& context, unsigned depth, unsigned size) :
        context(context), depth(depth), bindings(size) {}

    /// Unify `patterns` (a partial specialization's argument pattern or a
    /// primary template's injected arguments) against `arguments`. On success,
    /// every deduced parameter at `depth` is recorded in `bindings`.
    bool unify(TemplateArguments patterns, TemplateArguments arguments);

    /// The deduced arguments, indexed by parameter index. Unbound parameters
    /// hold a null TemplateArgument.
    llvm::ArrayRef<clang::TemplateArgument> results() const {
        return bindings;
    }

private:
    bool unify(const clang::TemplateArgument& pattern, const clang::TemplateArgument& argument);

    bool unify(clang::QualType pattern, clang::QualType argument);

    /// Extract a template-id (template + arguments) view of `type`, looking
    /// through TST, ClassTemplateSpecializationDecl records and injected class
    /// names. Returns false if `type` is not a template-id.
    bool template_id(clang::QualType type,
                     clang::TemplateName& name,
                     TemplateArguments& arguments) const;

    bool bind(unsigned index, const clang::TemplateArgument& argument);

    clang::ASTContext& context;
    unsigned depth;
    llvm::SmallVector<clang::TemplateArgument, 4> bindings;
};

/// Deduce the arguments of `params` at its own depth by matching `patterns`
/// against `arguments`; then fill remaining parameters from default arguments
/// where representable (type defaults are returned still containing template
/// parameters — the caller substitutes them with its instantiation stack).
/// Returns false if any parameter ends up unbound and defaultless.
///
/// `patterns` and `params` come in the same pairings the resolver already
/// uses: injected arguments for primary templates and alias templates,
/// `getTemplateArgs()` for partial specializations.
bool deduce_arguments(clang::ASTContext& context,
                      clang::TemplateParameterList* params,
                      llvm::ArrayRef<clang::TemplateArgument> patterns,
                      llvm::ArrayRef<clang::TemplateArgument> arguments,
                      llvm::SmallVectorImpl<clang::TemplateArgument>& deduced);

/// Partial ordering via symmetric deduction: `left` is more specialized than
/// `right` iff right's pattern matches left's and not vice versa.
bool more_specialized(clang::ASTContext& context,
                      clang::ClassTemplatePartialSpecializationDecl* left,
                      clang::ClassTemplatePartialSpecializationDecl* right);

/// If `expr` is a (possibly parenthesized/casted) reference to a non-type
/// template parameter, return its declaration.
const clang::NonTypeTemplateParmDecl* referenced_nttp(const clang::Expr* expr);

}  // namespace clice
