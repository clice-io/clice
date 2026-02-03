#pragma once

#include <vector>
#include <cstdint>

#include "AST/SourceCode.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

struct CompilationParams;

namespace config {

struct DefinitionOption {
    /// Insert placeholder for keywords? function call parameters? template arguments?
    bool enable_keyword_snippet = false;

    /// Also apply for lambda ...
    bool enable_function_arguments_snippet = false;
    bool enable_template_arguments_snippet = false;

    bool insert_paren_in_function_call = false;
    /// TODO: Add more detailed option, see
    /// https://github.com/llvm/llvm-project/issues/63565

    bool bundle_overloads = true;

    /// The limits of code completion, 0 is non limit.
    std::uint32_t limit = 0;
};

};  // namespace config

namespace feature {

enum class CompletionItemKind {
    None = 0,
    Text,
    Method,
    Function,
    Constructor,
    Field,
    Variable,
    Class,
    Interface,
    Module,
    Property,
    Unit,
    Value,
    Enum,
    Keyword,
    Snippet,
    Color,
    File,
    Reference,
    Folder,
    EnumMember,
    Constant,
    Struct,
    Event,
    Operator,
    TypeParameter
};

/// Represents a single code completion item to be presented to the user.
struct CompletionItem {
    /// The primary label displayed in the completion list.
    std::string label;

    /// Additional details, like a function signature, shown next to the label.
    std::string detail;

    /// A short description of the item, typically its type or namespace.
    std::string description;

    /// Full documentation for the item, shown on selection or hover.
    std::string document;

    /// The kind of item (function, class, etc.), used for an icon.
    CompletionItemKind kind;

    /// A score for ranking this item against others. Higher is better.
    float score;

    /// Whether this item is deprecated (often rendered with a strikethrough).
    bool deprecated;

    /// The text edit to be applied when this item is accepted.
    struct Edit {
        /// The new text to insert, which may be a snippet.
        std::string text;

        /// The source range to be replaced by the new text.
        LocalSourceRange range;
    } edit;
};

using CodeCompletionResult = std::vector<CompletionItem>;

std::vector<CompletionItem> definition(CompilationParams& params,
                                       const config::DefinitionOption& option);

}  // namespace feature

}  // namespace clice

