module;

/// The names of the files a completion's compile loads — a precompiled
/// preamble, the modules it imports — when it reads no others of them
/// (CompileCompletion::by_name): which of them it loads, and where
/// qualified lookup reaches past the names the index offers.

module clice:feature.code_completion.names;

import :feature.code_completion;
import :support.prelude;

namespace clice::feature {

/// What load_names loaded.
struct LoadedNames {
    /// The namespace a qualifier names, null without one.
    clang::DeclContext* space = nullptr;

    /// More names of the loaded files match the typed text than it read.
    bool more = false;
};

/// Loads, of the files the compile loads, what Sema lists at the point
/// while it reads no other names of namespaces (CompileCompletion::
/// by_name): the candidates' names and, of the identifiers the typed name
/// matches, the best scope_candidate_limit that name something there — the declarations
/// unqualified lookup finds, or those of the namespace a qualifier names,
/// and the macros. With nothing typed, a scope the index holds no more of
/// than the candidates is read whole, and so is a namespace the index
/// searched under another name, through an alias.
LoadedNames load_names(clang::Sema& sema, const CompileCompletion& names);

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
