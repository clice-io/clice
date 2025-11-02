#pragma once

#include "AST/SourceCode.h"
#include "AST/SymbolKind.h"
#include "Index/Shared.h"
#include "Protocol/Basic.h"

namespace clice::config {

struct HoverOptions {
    /// Strip doxygen info and merge with lsp info
    bool enable_doxygen_parsing = true;
    /// If set `false`, the comment will be wrapped
    /// in code block and keep ascii typesetting
    bool parse_comment_as_markdown = true;
    /// Show sugar type
    bool show_aka = true;
};

}  // namespace clice::config

namespace clice::feature {

struct HoverItem {
    enum class HoverKind : uint8_t {
        /// The typename of a variable or a type alias.
        Type,
        /// Size of type or variable.
        Size,
        /// Align of type or variable.
        Align,
        /// Offset of field in a class/struct.
        Offset,
        /// Bit width of a bit field.
        BitWidth,
        /// The index of a field in a class/struct.
        FieldIndex,
        /// The value of variable(on initialization / constant) | enum item
        Value,
    };

    using enum HoverKind;

    HoverKind kind;

    std::string value;
};

/// Hover information for a symbol.
struct Hover {
    /// Title
    SymbolKind kind;

    std::string name;

    /// Extra information.
    std::vector<HoverItem> items;

    /// Raw document in the source code.
    std::string document;

    /// The full qualified name of the declaration.
    std::string qualifier;

    /// The source code of the declaration.
    std::string source;

    /// Highlight range
    std::optional<proto::Range> hl_range;

    std::optional<std::string> get_item_content(HoverItem::HoverKind kind);

    /// Return the markdown string of hover info
    std::optional<std::string> display(config::HoverOptions opt);
};

std::optional<Hover> hover(CompilationUnitRef unit,
                           std::uint32_t offset,
                           const config::HoverOptions& opt);

}  // namespace clice::feature
