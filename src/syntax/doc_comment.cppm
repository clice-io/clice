module;

#include "modules/prelude.h"

module clice:syntax.doc_comment;

import :syntax.lexical_scan;
import :syntax.token;

namespace clice {

/// A declaration as the search for its comment sees it: where it sits in
/// its file.
struct DocumentedDecl {
    /// From the declaration's first token through its last: a template
    /// declares from its `template` keyword, a macro from its `#define`.
    LocalSourceRange range;

    /// Whether a comment after it on its last line documents it, as one
    /// does a field, an enumerator, a variable or a macro.
    bool trailing = false;
};

/// A macro as the search for its comment sees it: its definition (from
/// its name through its last token) from the `#` of its `#define` on.
DocumentedDecl documented_macro(llvm::StringRef content, LocalSourceRange definition);

/// Whether a comment's text is Doxygen's: `///`, `//!`, `/**` or `/*!` —
/// a banner of slashes or stars is not.
bool is_doc_comment(llvm::StringRef text);

/// The comments of a file documenting a declaration, a run of `comments`
/// (the file's, in order): a comment after the declaration on its last
/// line when the declaration takes one and the comment is marked as such
/// (`///<`); else the comments ending right above it, as clang attaches
/// them — nothing but a declaration's leading tokens between: none of
/// `;{}#`, no label such as `public:` and no list left open such as
/// `template <` — except that a plain `//` or `/* */` comment a blank line
/// separates documents nothing; else an unmarked comment after it. A
/// comment following code on its line documents only that code. The run
/// takes in the comments of the same style on the lines next to it. Empty
/// when no comment documents the declaration.
llvm::ArrayRef<LexicalInfo::Comment>
    documenting_comments(llvm::StringRef content,
                         llvm::ArrayRef<LexicalInfo::Comment> comments,
                         DocumentedDecl decl);

/// Of the comments documenting the declarations of one entity, offered in
/// order, the one documenting the entity: the first Doxygen comment, else
/// the first of any style. Once a Doxygen comment is offered the choice
/// stays.
class DocCommentChoice {
public:
    /// Offer the comments documenting one declaration of `content`
    /// (documenting_comments); true once the choice is made.
    bool offer(llvm::StringRef content, llvm::ArrayRef<LexicalInfo::Comment> run);

    bool decided() const {
        return doxygen;
    }

    bool empty() const {
        return text.empty();
    }

    /// The text (comment_text) of the comment chosen, empty without one.
    std::string take() && {
        return std::move(text);
    }

private:
    std::string text;
    bool doxygen = false;
};

/// The text of a run of comments: the comment markers and the leading `*`
/// of a block comment's lines taken out, the indentation the lines share
/// removed and the rest of it kept, banner lines at either end dropped.
std::string comment_text(llvm::StringRef content, llvm::ArrayRef<LexicalInfo::Comment> comments);

}  // namespace clice
