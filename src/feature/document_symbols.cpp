#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/feature.h"
#include "semantic/decls.h"
#include "semantic/display.h"
#include "semantic/semantics.h"
#include "semantic/symbol.h"

#include "llvm/Support/Casting.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/PrettyPrinter.h"

namespace clice::feature {

namespace {

auto to_protocol_symbol_kind(SymbolKind kind) -> protocol::SymbolKind {
    using enum protocol::SymbolKind;

    switch(kind) {
        case SymbolKind::Module: return Module;
        case SymbolKind::Namespace: return Namespace;
        case SymbolKind::Class: return Class;
        case SymbolKind::Struct: return Struct;
        case SymbolKind::Union: return Class;
        case SymbolKind::Enum: return Enum;
        // Type aliases are the only Type producer in the outline; clangd
        // maps them to Class as well.
        case SymbolKind::Type: return Class;
        case SymbolKind::Concept: return TypeParameter;
        case SymbolKind::Field: return Field;
        case SymbolKind::EnumMember: return EnumMember;
        case SymbolKind::Function: return Function;
        case SymbolKind::Method: return Method;
        case SymbolKind::Variable:
        case SymbolKind::Parameter:
        case SymbolKind::Label:
        case SymbolKind::Keyword:
        case SymbolKind::Directive:
        case SymbolKind::MacroParameter:
        case SymbolKind::Attribute: return Variable;
        case SymbolKind::Macro: return Function;
        case SymbolKind::Comment:
        case SymbolKind::Character:
        case SymbolKind::String:
        case SymbolKind::Header: return String;
        case SymbolKind::Number: return Number;
        case SymbolKind::Operator:
        case SymbolKind::Paren:
        case SymbolKind::Bracket:
        case SymbolKind::Brace:
        case SymbolKind::Angle: return Operator;
        case SymbolKind::Conflict:
        case SymbolKind::Invalid: return Variable;
    }

    return Variable;
}

auto symbol_detail(clang::ASTContext& context, const clang::NamedDecl& decl) -> std::string {
    display::Options options = {
        .suppress_scope = true,
        .suppress_unwritten_scope = true,
        .polish_for_declaration = true,
    };

    std::string detail;
    llvm::raw_string_ostream stream(detail);

    if(decl.getDescribedTemplateParams()) {
        stream << "template ";
    }

    if(const auto* value = llvm::dyn_cast<clang::ValueDecl>(&decl)) {
        if(llvm::isa<clang::CXXConstructorDecl>(value)) {
            std::string type = display::type(context, value->getType(), options).text;
            llvm::StringRef without_void = type;
            without_void.consume_front("void ");
            stream << without_void;
        } else if(!llvm::isa<clang::CXXDestructorDecl>(value)) {
            stream << display::type(context, value->getType(), options).text;
        }
    } else if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(&decl)) {
        stream << tag->getKindName();
    } else if(llvm::isa<clang::TypedefNameDecl>(&decl)) {
        stream << "type alias";
    } else if(llvm::isa<clang::ConceptDecl>(&decl)) {
        stream << "concept";
    }

    return detail;
}

/// Collects the outline by walking the unit's cached Semantics node table —
/// the DFS pre-order record of the interested file's written AST — instead of
/// running another RecursiveASTVisitor over the TU. Nesting comes for free:
/// a symbol's frame stays open while the walk index is inside its subtree.
class Collector {
public:
    explicit Collector(CompilationUnitRef unit) : unit(unit) {}

    auto collect() -> std::vector<DocumentSymbol> {
        auto nodes = unit.semantics().node_entries();
        std::uint32_t index = 0;
        while(index < nodes.size()) {
            const Semantics::Node& entry = nodes[index];
            if(!entry.node.is_ast()) {
                // The preprocessor segment follows the AST segment; nothing
                // there produces an outline symbol.
                break;
            }

            close_frames(index);

            if(entry.node.kind() == SemanticNode::Kind::Decl) {
                if(!handle_decl(entry.node.get<clang::Decl>(), entry.subtree_end)) {
                    index = entry.subtree_end;
                    continue;
                }
            }

            index += 1;
        }

        return std::move(symbols);
    }

private:
    /// Restore the insertion cursor of every symbol whose subtree the walk
    /// has left.
    void close_frames(std::uint32_t index) {
        while(!frames.empty() && index >= frames.back().subtree_end) {
            cursor = frames.back().previous;
            frames.pop_back();
        }
    }

    /// Returns false when the decl's whole subtree should be skipped.
    bool handle_decl(const clang::Decl* decl, std::uint32_t subtree_end) {
        const auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
        if(!named) {
            return true;
        }

        if(decls::is_implicit_instantiation(named)) {
            return false;
        }

        // Explicit instantiations carry no written body. The class form
        // (`template struct Box<int>;`) gets a childless outline node — its
        // members are instantiated decls located in the primary template.
        // Clang records function and variable instantiations (and their
        // instantiated members) at the primary's location, so those produce
        // no symbol at all (mirroring resolve_occurrences).
        bool childless_instantiation = false;
        if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
            if(!llvm::isa<clang::ClassTemplatePartialSpecializationDecl>(spec) &&
               clang::isTemplateInstantiation(spec->getSpecializationKind())) {
                childless_instantiation = true;
            }
        } else if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            if(clang::isTemplateInstantiation(function->getTemplateSpecializationKind())) {
                return false;
            }
        } else if(const auto* var = llvm::dyn_cast<clang::VarDecl>(decl)) {
            if(clang::isTemplateInstantiation(var->getTemplateSpecializationKind())) {
                return false;
            }
        }

        if(!is_interested(decl)) {
            return true;
        }

        // A function's declaration name may span several tokens
        // (`~Widget`, `operator bool`, `operator==`); NameInfo covers them
        // all, while getLocation() is only the first one.
        clang::SourceRange name_range = named->getLocation();
        if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(named)) {
            name_range = function->getNameInfo().getSourceRange();
        }

        // Names spelled inside a macro argument (`DEFINE(name)`) select the
        // written spelling; names spelled in the macro body keep the
        // invocation site.
        name_range = clang::SourceRange(unit.file_location(name_range.getBegin()),
                                        unit.file_location(name_range.getEnd()));

        auto [fid, selection_range] = unit.decompose_range(name_range);
        auto [fid2, range] = unit.decompose_expansion_range(named->getSourceRange());
        if(fid != fid2 || fid != unit.interested_file() || !selection_range.valid() ||
           !range.valid()) {
            return false;
        }

        // LSP requires the selection range to be contained in the full
        // range; a macro invocation's expansion range may be narrower than
        // the name spelled in its argument.
        range.begin = std::min(range.begin, selection_range.begin);
        range.end = std::max(range.end, selection_range.end);

        auto& symbol = cursor->emplace_back();
        symbol.kind = SymbolKind::from(decl);
        symbol.name = display::name_of(named);
        symbol.detail = symbol_detail(unit.context(), *named);
        symbol.selection_range = selection_range;
        symbol.range = range;

        if(childless_instantiation) {
            return false;
        }

        frames.push_back({subtree_end, cursor});
        cursor = &symbol.children;
        return true;
    }

    static bool is_interested(const clang::Decl* decl) {
        switch(decl->getKind()) {
            case clang::Decl::Namespace:
            case clang::Decl::Enum:
            case clang::Decl::EnumConstant:
            case clang::Decl::Function:
            case clang::Decl::CXXMethod:
            case clang::Decl::CXXConstructor:
            case clang::Decl::CXXDestructor:
            case clang::Decl::CXXConversion:
            case clang::Decl::CXXDeductionGuide:
            case clang::Decl::Record:
            case clang::Decl::CXXRecord:
            case clang::Decl::ClassTemplateSpecialization:
            case clang::Decl::ClassTemplatePartialSpecialization:
            case clang::Decl::Field:
            case clang::Decl::Var:
            case clang::Decl::VarTemplateSpecialization:
            case clang::Decl::VarTemplatePartialSpecialization:
            case clang::Decl::Binding:
            case clang::Decl::Typedef:
            case clang::Decl::TypeAlias:
            case clang::Decl::Concept: return true;
            default: return false;
        }
    }

    struct Frame {
        std::uint32_t subtree_end;
        std::vector<DocumentSymbol>* previous;
    };

    CompilationUnitRef unit;
    std::vector<DocumentSymbol> symbols;
    std::vector<DocumentSymbol>* cursor = &symbols;
    std::vector<Frame> frames;
};

void sort_symbols(std::vector<DocumentSymbol>& symbols) {
    std::ranges::sort(symbols, [](const DocumentSymbol& lhs, const DocumentSymbol& rhs) {
        if(lhs.range.begin != rhs.range.begin) {
            return lhs.range.begin < rhs.range.begin;
        }
        return lhs.range.end < rhs.range.end;
    });

    for(auto& symbol: symbols) {
        sort_symbols(symbol.children);
    }
}

auto to_protocol_symbol(const DocumentSymbol& symbol, const LineMap& map)
    -> std::optional<protocol::DocumentSymbol> {
    auto range = to_range(map, symbol.range);
    auto selection_range = to_range(map, symbol.selection_range);
    if(!range || !selection_range)
        return std::nullopt;

    protocol::DocumentSymbol result{
        .name = symbol.name,
        .kind = to_protocol_symbol_kind(symbol.kind),
        .range = *range,
        .selection_range = *selection_range,
    };

    if(!symbol.detail.empty()) {
        result.detail = symbol.detail;
    }

    if(!symbol.children.empty()) {
        std::vector<std::shared_ptr<protocol::DocumentSymbol>> children;
        children.reserve(symbol.children.size());
        for(const auto& child: symbol.children) {
            if(auto converted = to_protocol_symbol(child, map)) {
                children.push_back(
                    std::make_shared<protocol::DocumentSymbol>(std::move(*converted)));
            }
        }
        result.children = std::move(children);
    }

    return result;
}

}  // namespace

auto document_symbols(CompilationUnitRef unit) -> std::vector<DocumentSymbol> {
    auto result = Collector(unit).collect();
    sort_symbols(result);
    return result;
}

auto document_symbols(CompilationUnitRef unit, PositionEncoding encoding)
    -> std::vector<protocol::DocumentSymbol> {
    auto internal = document_symbols(unit);
    LineMap map(unit.interested_content(), unit.line_starts(), encoding);

    std::vector<protocol::DocumentSymbol> symbols;
    symbols.reserve(internal.size());

    for(const auto& symbol: internal) {
        if(auto converted = to_protocol_symbol(symbol, map)) {
            symbols.push_back(std::move(*converted));
        }
    }

    return symbols;
}

}  // namespace clice::feature
