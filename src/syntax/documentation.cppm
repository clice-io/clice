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

/// The documentation of what a `@copydoc`, `@copybrief` or `@copydetails`
/// names, the names in its own comment looked up from where it is
/// declared; nullopt when the name resolves to nothing documented.
using CopiedDocLookup = llvm::function_ref<std::optional<Documentation>(llvm::StringRef name)>;

/// Read the text of a documentation comment as Doxygen does: its commands
/// with `@` or `\`, the Markdown it allows, and GTK-Doc's and kernel-doc's
/// conventions — the name line, `@param:` lines and the `Returns:`
/// section. An unknown command stays as written. A comment documenting a
/// file, a page or a group documents no declaration: empty.
Documentation parse_documentation(llvm::StringRef text, CopiedDocLookup lookup);

/// Whether `name`, written after `@` or `\` in a documentation comment, is a
/// command parse_documentation reads.
bool is_doc_command(llvm::StringRef name);

/// A name as `@copydoc` writes it: `name`, `Class::member`,
/// `Class#member`, `::name`, and for one overload of a function its
/// parameter types, `f(int, const char*)`.
struct CopiedName {
    /// The scopes, outermost first, then the name.
    llvm::SmallVector<llvm::StringRef, 4> parts;

    /// Written with a leading `::`: a name of the global scope.
    bool absolute = false;

    /// What the parentheses after the name hold.
    std::optional<llvm::StringRef> parameters;
};

/// nullopt when a scope or the name is empty, as in `A::` or `#f`.
std::optional<CopiedName> parse_copied_name(llvm::StringRef name);

/// Whether the function whose name ends at `name_end` in `content` declares
/// the parameters `spelled` lists: the same types in the same order, a
/// name only one side gives and the default arguments aside.
bool declares_parameters(llvm::StringRef content, std::uint32_t name_end, llvm::StringRef spelled);

}  // namespace clice
