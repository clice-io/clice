module clice:feature.documentation;

import :compile.compilation;
import :index.query;
import :index.symbol;
import :support.markup;
import :support.prelude;
import :syntax.documentation;
import :vfs.ids;

namespace clice::feature {

/// Corresponds to the `[documentation]` section in clice.toml.
KANNOTATE_TYPE(struct DocumentationOptions, presence_all = optional);
struct DocumentationOptions {
    KANNOTATE(&format,
              choices = {"doxygen", "plain"},
              description =
                  "How documentation comments are read, wherever they "
                  "show (hover, completion, signature help, `clice "
                  "query`): \"doxygen\" reads Doxygen's commands "
                  "(`@brief`, `@param`, `@return`, ...) and the "
                  "Markdown Doxygen allows, along with the GTK-Doc and "
                  "kernel-doc conventions; \"plain\" shows a comment as "
                  "written.")
    std::string format = "doxygen";

    KANNOTATE(&inherit,
              description =
                  "An overriding method without a documentation comment "
                  "shows the one of the method it overrides.")
    bool inherit = true;
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

/// Add what a card shows of a documentation first: that the entity is
/// deprecated, with the message of its attribute (`attribute`) or of its
/// comment, then the brief.
void add_summary(markup::Document& output,
                 const Documentation& documentation,
                 std::optional<llvm::StringRef> attribute);

/// Add what a documentation says of the template parameters, the
/// parameters, what the function returns — in general, then value by value
/// — and the exceptions it throws, each under its title; nothing for what
/// it says nothing of.
void add_sections(markup::Document& output, const Documentation& documentation);

/// Add what a card shows of a documentation last: its details, then what
/// it refers to (`@see`).
void add_details(markup::Document& output, const Documentation& documentation);

/// A documentation on its own, for a reader without the declaration at
/// hand: its brief, its parameters, what it returns and throws, then its
/// details.
auto present(const Documentation& documentation) -> markup::Document;

}  // namespace clice::feature
