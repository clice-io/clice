module;

#include "modules/prelude.h"

module clice:syntax.documentation;

namespace clice {

/// A documentation comment split by what each part of it documents. The
/// texts are Markdown: the Markdown a comment is written in passes through,
/// Doxygen's inline commands become their Markdown equivalents.
struct Documentation {
    struct Param {
        bool operator==(const Param&) const = default;

        std::string name;

        /// `in`, `out` or `in,out`, as the command gave it; empty when it
        /// gave none.
        std::string direction;

        std::string text;
    };

    /// What the comment says of one value a function returns (`@retval`)
    /// or of one exception it throws (`@throws`).
    struct Entry {
        bool operator==(const Entry&) const = default;

        std::string name;
        std::string text;
    };

    bool operator==(const Documentation&) const = default;

    /// The summary: what `@brief` says, else the first paragraph when a
    /// blank line or a command ends it.
    std::string brief;

    /// The rest of the description in the order it is written:
    /// paragraphs, lists, tables, code blocks, notes and warnings.
    std::string details;

    std::vector<Param> params;
    std::vector<Param> template_params;
    std::string returns;
    std::vector<Entry> retvals;
    std::vector<Entry> throws;

    /// Set by `@deprecated`, to what it says.
    std::optional<std::string> deprecated;

    /// What each `@see` names.
    std::vector<std::string> see;

    /// `details` is the comment as written, to show without markup.
    bool verbatim = false;

    bool empty() const;

    const Param* param(llvm::StringRef name) const;

    const Param* template_param(llvm::StringRef name) const;
};

/// The comment text (comment_text) documenting what a `@copydoc`,
/// `@copybrief` or `@copydetails` names; nullopt when the name resolves to
/// nothing.
using CopiedDocLookup = llvm::function_ref<std::optional<std::string>(llvm::StringRef name)>;

/// Read the text of a documentation comment as Doxygen does: its commands
/// with `@` or `\`, the Markdown it allows, and GTK-Doc's and kernel-doc's
/// conventions — the name line, `@param:` lines and the `Returns:`
/// section. An unknown command stays as written. A comment documenting a
/// file, a page or a group documents no declaration: empty.
Documentation parse_documentation(llvm::StringRef text, CopiedDocLookup lookup);

}  // namespace clice
