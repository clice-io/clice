module;

#include "support/prelude.h"

module clice:feature.documentation;

import :compile.compilation;
import :index.query;
import :index.symbol;
import :support.markup;
import :syntax.documentation;
import :vfs.ids;

namespace clice::feature {

/// Corresponds to the `[documentation]` section in clice.toml.
struct DocumentationOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     choices = {"doxygen", "plain"},
                     description =
                         "How documentation comments are read, wherever they "
                         "show (hover, completion, signature help, `clice "
                         "query`): \"doxygen\" reads Doxygen's commands "
                         "(`@brief`, `@param`, `@return`, ...) and the "
                         "Markdown Doxygen allows, along with the GTK-Doc and "
                         "kernel-doc conventions; \"plain\" shows a comment as "
                         "written.")
    <std::string> format = "doxygen";

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "An overriding method without a documentation comment "
                         "shows the one of the method it overrides.")
    <bool> inherit = true;
};

/// The documentation of a declaration: its comment (doc_comment) read as
/// `options` say.
auto documentation(CompilationUnit& unit,
                   const clang::NamedDecl* decl,
                   const DocumentationOptions& options) -> Documentation;

/// The same of a macro.
auto documentation(CompilationUnit& unit,
                   const clang::MacroInfo* macro,
                   const DocumentationOptions& options) -> Documentation;

/// The same of an indexed symbol, from the comments in the text the index
/// read (index::doc_comment).
auto documentation(const index::IndexQuery& query,
                   index::SymbolHash hash,
                   Fid anchor,
                   const DocumentationOptions& options) -> Documentation;

/// Write after an entry — a parameter, a return value, an exception — what
/// a documentation says of it: its direction, then its text.
void describe(markup::Paragraph& entry, llvm::StringRef direction, llvm::StringRef text);

/// Add what a card shows of a documentation first: that the entity is
/// deprecated, with the message of its attribute (`attribute`) or of its
/// comment, then the brief.
void add_summary(markup::Document& output,
                 const Documentation& documentation,
                 std::optional<llvm::StringRef> attribute);

/// Add the parameters a comment documents under `title`, each with what it
/// says of it; nothing when it documents none.
void add_parameters(markup::Document& output,
                    llvm::StringRef title,
                    llvm::ArrayRef<Documentation::Param> documented);

/// Add what a documentation says of the values a function returns or the
/// exceptions it throws, as a list under `title` (none when empty).
void add_entries(markup::Document& output,
                 llvm::StringRef title,
                 llvm::ArrayRef<Documentation::Entry> entries);

/// Add what a card shows of a documentation last: its details, then what
/// it refers to (`@see`).
void add_details(markup::Document& output, const Documentation& documentation);

/// A documentation on its own, for a reader without the declaration at
/// hand: its brief, its parameters, what it returns and throws, then its
/// details.
auto present(const Documentation& documentation) -> markup::Document;

}  // namespace clice::feature
