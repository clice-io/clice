module;

/// The precompiled preamble's names in a completion's compile that reads
/// no others of them (CompileCompletion::from_index): which of them the
/// compile loads, and where qualified lookup reaches past the names the
/// index offers.

module clice:feature.code_completion.names;

import :feature.code_completion;
import :index.search;
import :support.prelude;

namespace clice::feature {

/// Loads, of the precompiled preamble, what Sema lists at the point of
/// the candidates' names while it reads no other names of namespaces
/// (CompileCompletion::from_index): the declarations unqualified lookup
/// finds, or those of the namespace a qualifier names. A namespace the
/// index searched under another name, through an alias, is read whole.
/// The namespace a qualifier names, null without one.
clang::DeclContext* load_names(clang::Sema& sema,
                               const index::CompletionScope& scope,
                               llvm::ArrayRef<ScopeCandidate> candidates);

/// Whether qualified lookup in `space` finds names the index files under
/// other scopes: those a using-declaration or using-directive brings in,
/// and those of C linkage, in it or in an inline or anonymous namespace
/// inside. Read from the kinds of its declarations, which loads no others.
bool brings_in(clang::ASTContext& ast, const clang::DeclContext* space);

/// Reads whole the names qualified lookup finds in `space`: its own, and
/// those of the namespaces its using-directives name, anonymous ones among
/// them.
void read_names(clang::DeclContext* space);

}  // namespace clice::feature
