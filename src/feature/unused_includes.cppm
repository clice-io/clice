module clice:feature.unused_includes;

import :compile.compilation;
import :feature.code_actions;
import :feature.diagnostics;
import :index.tu_index;
import :support.prelude;
import :syntax.token;

namespace clice::feature {

/// The `#include`s of the focus file of a whole compile it can do without,
/// all at once, as warnings tagged unnecessary: those whose header brings
/// nothing the file needs that the ones kept do not bring too. An
/// `#include` of a header declaring a name the file uses stays, as does the
/// standard library's header of a name of its; the others stay in source
/// order when they bring something no kept one brings, so a header the file
/// reaches what it uses through stays. What the file uses is what its code
/// names, what the templates it instantiates name, the macros it expands
/// and the definitions a header pastes into it; what a header names but
/// neither declares nor includes a declaration of is a need of the file's
/// too, before the `#include` bringing that header. `index` is the
/// compile's TU index, every file's section included: its include tree and
/// what each header names. None for a header, whose includes may be there
/// for its includers, nor for a file with errors.
auto unused_includes(CompilationUnit& unit, const index::TUIndex& index)
    -> std::vector<FileDiagnostic>;

/// The fixes of the unused-include warnings among `diagnostics`, at bytes
/// of `text`: removing the line of each one `selection` meets, and when
/// there are several, removing all of them.
auto unused_include_fixes(llvm::ArrayRef<FileDiagnostic> diagnostics,
                          llvm::StringRef text,
                          LocalSourceRange selection) -> std::vector<ResolvedAction>;

}  // namespace clice::feature
