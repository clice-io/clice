/// The comments that document declarations: which comments a declaration
/// has (attachment), what Doxygen reads in them (doxygen), and the two read
/// over a compiled unit's AST.

module clice:semantic.comment;

import :compile.compilation;
import :semantic.comment.attachment;
import :semantic.comment.doxygen;
import :support.prelude;
import :syntax.lexical_scan;

namespace clice {

/// The comments attached to one declaration (attached_comments over its
/// file's text) and the file they are in.
struct AttachedComments {
    clang::FileID file;
    llvm::StringRef content;
    llvm::ArrayRef<LexicalInfo::Comment> comments;
};

/// None for what the compiler declares implicitly, a parameter, a template
/// parameter and a namespace: a namespace often has too many redeclarations
/// for any one's comment to be useful, and a file header passes for its
/// comment.
AttachedComments attached_comments(CompilationUnit& unit, const clang::Decl* decl);

/// A name a `@param` or `@tparam` writes, and the parameter it names.
struct DocumentedParameter {
    const clang::NamedDecl* parameter;
    clang::SourceLocation location;
};

/// The parameters and template parameters the comments attached to this one
/// declaration name with `@param` and `@tparam`, at each name they write.
llvm::SmallVector<DocumentedParameter> documented_parameters(CompilationUnit& unit,
                                                             const clang::Decl* decl);

}  // namespace clice
