module;

#include "modules/prelude.h"

module clice:semantic.documentation;

import :compile.compilation;

namespace clice {

/// The text (comment_text) of the comment documenting `decl`: of its
/// declarations, the first a Doxygen comment documents, else the first any
/// comment documents (documenting_comments); a template's instantiation and
/// its members read the template's. A namespace, a parameter and what the
/// compiler declares implicitly have none. With `inherit`, a method
/// overriding another without one takes that method's.
std::string doc_comment(CompilationUnit& unit, const clang::NamedDecl* decl, bool inherit);

/// The text of the comment documenting a macro: above its `#define`, or
/// after it on its line.
std::string doc_comment(CompilationUnit& unit, const clang::MacroInfo* macro);

/// The declarations a `@copydoc` in the comment of `decl` may name
/// (parse_copied_name): looked up from `decl`'s scope outward, or in the
/// global scope, a qualified name part by part; of several, the functions
/// declaring the parameters it lists, when it lists them.
llvm::SmallVector<const clang::NamedDecl*> copied_doc_targets(CompilationUnit& unit,
                                                              const clang::NamedDecl* decl,
                                                              llvm::StringRef name);

}  // namespace clice
