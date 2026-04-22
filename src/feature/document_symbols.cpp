#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "feature/feature.h"
#include "semantic/ast_utility.h"
#include "semantic/filtered_ast_visitor.h"
#include "semantic/symbol_kind.h"

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
        case SymbolKind::Type:
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
    clang::PrintingPolicy policy(context.getPrintingPolicy());
    policy.SuppressScope = true;
    policy.SuppressUnwrittenScope = true;
    policy.AnonymousTagLocations = false;
    policy.PolishForDeclaration = true;

    std::string detail;
    llvm::raw_string_ostream stream(detail);

    if(decl.getDescribedTemplateParams()) {
        stream << "template ";
    }

    if(const auto* value = llvm::dyn_cast<clang::ValueDecl>(&decl)) {
        if(llvm::isa<clang::CXXConstructorDecl>(value)) {
            std::string type = value->getType().getAsString(policy);
            llvm::StringRef without_void = type;
            without_void.consume_front("void ");
            stream << without_void;
        } else if(!llvm::isa<clang::CXXDestructorDecl>(value)) {
            value->getType().print(stream, policy);
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

struct InternalSymbol {
    std::string name;
    std::string detail;
    SymbolKind kind = SymbolKind::Invalid;
    LocalSourceRange range;
    LocalSourceRange selection_range;
    std::vector<InternalSymbol> children;
};

struct SymbolFrame {
    std::vector<InternalSymbol> symbols;
    std::vector<InternalSymbol>* cursor = &symbols;
};

class DocumentSymbolCollector : public FilteredASTVisitor<DocumentSymbolCollector> {
public:
    explicit DocumentSymbolCollector(CompilationUnitRef unit) : FilteredASTVisitor(unit, true) {}

    bool on_traverse_decl(clang::Decl* decl, auto traverse) {
        if(!is_interested(decl)) {
            return (this->*traverse)(decl);
        }

        auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
        if(!named) {
            return (this->*traverse)(decl);
        }

        auto [fid, selection_range] =
            unit.decompose_range(unit.expansion_location(named->getLocation()));
        auto [fid2, range] = unit.decompose_expansion_range(named->getSourceRange());
        if(fid != fid2 || fid != unit.interested_file() || !selection_range.valid() ||
           !range.valid()) {
            return true;
        }

        auto* previous = result.cursor;
        auto& symbol = result.cursor->emplace_back();
        symbol.kind = SymbolKind::from(decl);
        symbol.name = ast::display_name_of(named);
        symbol.detail = symbol_detail(unit.context(), *named);
        symbol.selection_range = selection_range;
        symbol.range = range;

        result.cursor = &symbol.children;
        auto ok = (this->*traverse)(decl);
        result.cursor = previous;
        return ok;
    }

    auto collect() -> std::vector<InternalSymbol> {
        TraverseDecl(unit.tu());
        return std::move(result.symbols);
    }

private:
    static bool is_interested(clang::Decl* decl) {
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
            case clang::Decl::Field:
            case clang::Decl::Var:
            case clang::Decl::Binding:
            case clang::Decl::Concept: return true;
            default: return false;
        }
    }

private:
    SymbolFrame result;
};

void sort_symbols(std::vector<InternalSymbol>& symbols) {
    std::ranges::sort(symbols, [](const InternalSymbol& lhs, const InternalSymbol& rhs) {
        if(lhs.range.begin != rhs.range.begin) {
            return lhs.range.begin < rhs.range.begin;
        }
        return lhs.range.end < rhs.range.end;
    });

    for(auto& symbol: symbols) {
        sort_symbols(symbol.children);
    }
}

auto to_protocol_symbol(const InternalSymbol& symbol, const PositionMapper& converter)
    -> protocol::DocumentSymbol {
    protocol::DocumentSymbol result{
        .name = symbol.name,
        .kind = to_protocol_symbol_kind(symbol.kind),
        .range = to_range(converter, symbol.range),
        .selection_range = to_range(converter, symbol.selection_range),
    };

    if(!symbol.detail.empty()) {
        result.detail = symbol.detail;
    }

    if(!symbol.children.empty()) {
        std::vector<std::shared_ptr<protocol::DocumentSymbol>> children;
        children.reserve(symbol.children.size());
        for(const auto& child: symbol.children) {
            children.push_back(
                std::make_shared<protocol::DocumentSymbol>(to_protocol_symbol(child, converter)));
        }
        result.children = std::move(children);
    }

    return result;
}

}  // namespace

auto document_symbols(CompilationUnitRef unit, PositionEncoding encoding)
    -> std::vector<protocol::DocumentSymbol> {
    auto internal = DocumentSymbolCollector(unit).collect();
    sort_symbols(internal);

    PositionMapper converter(unit.interested_content(), encoding);
    std::vector<protocol::DocumentSymbol> symbols;
    symbols.reserve(internal.size());

    for(const auto& symbol: internal) {
        symbols.push_back(to_protocol_symbol(symbol, converter));
    }

    return symbols;
}

}  // namespace clice::feature
