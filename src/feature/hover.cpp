#include <format>
#include <optional>
#include <string>

#include "feature/feature.h"
#include "semantic/ast_utility.h"
#include "semantic/selection.h"
#include "semantic/symbol_kind.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

namespace clice::feature {

namespace {

auto symbol_name(SymbolKind kind) -> llvm::StringRef {
    switch(kind) {
        case SymbolKind::Module: return "module";
        case SymbolKind::Namespace: return "namespace";
        case SymbolKind::Class: return "class";
        case SymbolKind::Struct: return "struct";
        case SymbolKind::Union: return "union";
        case SymbolKind::Enum: return "enum";
        case SymbolKind::Type: return "type";
        case SymbolKind::Concept: return "concept";
        case SymbolKind::Field: return "field";
        case SymbolKind::EnumMember: return "enum member";
        case SymbolKind::Function: return "function";
        case SymbolKind::Method: return "method";
        case SymbolKind::Variable: return "variable";
        case SymbolKind::Parameter: return "parameter";
        case SymbolKind::Macro: return "macro";
        default: return "symbol";
    }
}

auto hover_markdown(const clang::NamedDecl& decl) -> std::string {
    auto kind = SymbolKind::from(&decl);
    auto name = ast::name_of(&decl);
    return std::format("{}: {}", symbol_name(kind), name);
}

auto hover_range(CompilationUnitRef unit,
                 const clang::NamedDecl& decl,
                 const PositionMapper& converter) -> std::optional<protocol::Range> {
    auto [fid, range] = unit.decompose_expansion_range(decl.getSourceRange());
    if(fid != unit.interested_file() || !range.valid()) {
        return std::nullopt;
    }

    return protocol::Range{
        .start = *converter.to_position(range.begin),
        .end = *converter.to_position(range.end),
    };
}

auto build_hover(CompilationUnitRef unit, const clang::NamedDecl& decl, PositionEncoding encoding)
    -> protocol::Hover {
    PositionMapper converter(unit.interested_content(), encoding);

    protocol::MarkupContent content{
        .kind = protocol::MarkupKind::Markdown,
        .value = hover_markdown(decl),
    };

    protocol::Hover result{
        .contents = content,
    };

    if(auto range = hover_range(unit, decl, converter)) {
        result.range = *range;
    }

    return result;
}

}  // namespace

auto hover(CompilationUnitRef unit,
           const clang::NamedDecl* decl,
           const HoverOptions&,
           PositionEncoding encoding) -> std::optional<protocol::Hover> {
    if(!decl) {
        return std::nullopt;
    }

    return build_hover(unit, *decl, encoding);
}

auto hover(CompilationUnitRef unit,
           std::uint32_t offset,
           const HoverOptions& options,
           PositionEncoding encoding) -> std::optional<protocol::Hover> {
    auto tree = SelectionTree::create_right(unit, LocalSourceRange(offset, offset));
    auto* node = tree.common_ancestor();
    if(!node) {
        return std::nullopt;
    }

    if(const auto* decl = node->get<clang::NamedDecl>()) {
        return hover(unit, decl, options, encoding);
    }

    if(const auto* ref = node->get<clang::DeclRefExpr>()) {
        return hover(unit, ref->getDecl(), options, encoding);
    }

    return std::nullopt;
}

}  // namespace clice::feature
