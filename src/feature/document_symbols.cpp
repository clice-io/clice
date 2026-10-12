module;

#include "modules/prelude.h"

module clice;

import :compile.compilation_unit;
import :compile.semantics;
import :feature.feature;
import :semantic.display;
import :semantic.symbol;
import :syntax.lexer;

namespace clice::feature {

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
        case SymbolKind::Primitive:
        case SymbolKind::MacroParameter:
        case SymbolKind::Attribute: return Variable;
        // A `#pragma mark`, which clangd and Xcode list as a file.
        case SymbolKind::Directive: return File;
        // The vocabulary has no macro kind; Constant reads right for the
        // object-like majority and the parameter-list detail marks the rest.
        case SymbolKind::Macro: return Constant;
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
        case SymbolKind::Invalid:
        case SymbolKind::Identifier: return Variable;
    }

    return Variable;
}

namespace {

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
/// the DFS pre-order record of the main file's written AST — instead of
/// running another RecursiveASTVisitor over the TU. Nesting comes for free:
/// a symbol's frame stays open while the walk index is inside its subtree.
class Collector {
public:
    Collector(CompilationUnitRef unit, llvm::ArrayRef<std::uint32_t> inactive_regions) :
        unit(unit), inactive_regions(inactive_regions) {}

    auto collect() -> std::vector<DocumentSymbol> {
        auto nodes = unit.semantics().node_entries();
        std::uint32_t index = 0;
        while(index < nodes.size()) {
            const Semantics::Node& entry = nodes[index];
            if(!entry.node.is_ast()) {
                // The preprocessor segment follows the AST segment.
                break;
            }

            close_frames(index);

            // Implicit instantiations (and anything below an instantiation
            // head) are located at the pattern, not here.
            if(entry.flags.in_instantiation) {
                index = entry.subtree_end;
                continue;
            }

            if(entry.node.kind() == SemanticNode::Kind::Decl) {
                if(!handle_decl(entry.node.get<clang::Decl>(), entry.subtree_end)) {
                    index = entry.subtree_end;
                    continue;
                }
            }

            index += 1;
        }

        for(; index < nodes.size(); index += 1) {
            auto& node = nodes[index].node;
            if(node.kind() == SemanticNode::Kind::Module) {
                add_module(*node.get<LexicalInfo::ModuleDeclaration>());
            }
        }

        for(auto directive: unit.semantics().macro_definitions()) {
            if(!inactive(directive.begin)) {
                add_macro(directive);
            }
        }

        add_marks();

        return std::move(symbols);
    }

private:
    bool inactive(std::uint32_t offset) const {
        auto bounds = std::ranges::upper_bound(inactive_regions, offset) - inactive_regions.begin();
        return bounds % 2 == 1;
    }

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
        // An explicit instantiation directive outlines as a childless symbol
        // of its specialization: the instantiated members sit at the pattern.
        if(const auto* directive = llvm::dyn_cast<clang::ExplicitInstantiationDecl>(decl)) {
            add_symbol(*directive->getSpecialization(),
                       directive->getNameLoc(),
                       directive->getSourceRange());
            return false;
        }

        // A friend declaration names an entity of the enclosing scope; only
        // a hidden friend's definition lives nowhere else in the outline.
        if(const auto* friend_decl = llvm::dyn_cast<clang::FriendDecl>(decl)) {
            const auto* function = friend_decl->getFriendDecl()
                                       ? friend_decl->getFriendDecl()->getAsFunction()
                                       : nullptr;
            return function && function->isThisDeclarationADefinition();
        }

        const auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
        if(!named || !is_supported(decl)) {
            return true;
        }

        // A function's declaration name may span several tokens
        // (`~Widget`, `operator bool`, `operator==`); NameInfo covers them
        // all, while getLocation() is only the first one.
        clang::SourceRange name_range = named->getLocation();
        if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(named)) {
            name_range = function->getNameInfo().getSourceRange();
        }

        DocumentSymbol* symbol = add_symbol(*named, name_range, named->getSourceRange());
        if(!symbol) {
            // A fragment's members outline at its top level.
            return unit.encloses_main_file(decl);
        }

        frames.push_back({subtree_end, cursor});
        cursor = &symbol->children;
        return true;
    }

    /// Appends the symbol at the cursor; nullptr when a range falls outside
    /// the main file.
    auto add_symbol(const clang::NamedDecl& named,
                    clang::SourceRange name_range,
                    clang::SourceRange full_range) -> DocumentSymbol* {
        // Names spelled inside a macro argument (`DEFINE(name)`) select the
        // written spelling; names spelled in the macro body keep the
        // invocation site.
        name_range = clang::SourceRange(unit.file_location(name_range.getBegin()),
                                        unit.file_location(name_range.getEnd()));

        // Clang leaves a bound it cannot locate unset, on valid code too:
        // `short __attribute__((vector_size(16)))`, `Ts...[0]`, an unclosed
        // `namespace a {`, the typeless `for(x : v)`.
        if(full_range.getBegin().isInvalid()) {
            full_range.setBegin(name_range.getBegin());
        }
        if(full_range.getEnd().isInvalid()) {
            full_range.setEnd(name_range.getEnd());
        }

        auto [fid, selection_range] = unit.decompose_range(name_range);
        auto [fid2, range] = unit.decompose_expansion_range(full_range);
        if(fid != fid2 || fid != unit.main_file() || !selection_range.valid() || !range.valid()) {
            return nullptr;
        }

        // LSP requires the selection range to be contained in the full
        // range; a macro invocation's expansion range may be narrower than
        // the name spelled in its argument.
        range.begin = std::min(range.begin, selection_range.begin);
        range.end = std::max(range.end, selection_range.end);

        auto& symbol = cursor->emplace_back();
        symbol.kind = SymbolKind::from(&named);
        symbol.name = display::name_of(&named);
        symbol.detail = symbol_detail(unit.context(), named);
        symbol.selection_range = selection_range;
        symbol.range = range;
        symbol.deprecated = named.isDeprecated();
        return &symbol;
    }

    /// A `#define` written in this file, read from its text: the
    /// preamble's have no preprocessor record in a compile under a PCH. The
    /// AST walk never sees directives, so nesting runs by containment
    /// instead: one spelled between class members outlines under the class.
    void add_macro(LocalSourceRange directive) {
        auto content = unit.main_content();
        auto lexer =
            Lexer::from_line(content, directive.begin, {.lang_opts = &unit.lang_options()});
        lexer.advance();
        lexer.advance();
        auto name = lexer.advance();
        if(!name.is_identifier()) {
            return;
        }

        auto spelling = [&](const Token& token) {
            return unit.token_spelling(unit.create_location(unit.main_file(), token.range.begin));
        };

        DocumentSymbol symbol;
        symbol.kind = SymbolKind::Macro;
        symbol.name = spelling(name);
        symbol.selection_range = name.range;

        auto token = lexer.advance();
        if(token.kind == clang::tok::l_paren && token.range.begin == name.range.end) {
            std::string detail = "(";
            for(token = lexer.advance(); token.kind != clang::tok::r_paren && !token.is_eod();
                token = lexer.advance()) {
                detail += token.kind == clang::tok::comma ? ", " : spelling(token);
            }
            detail += ')';
            symbol.detail = std::move(detail);
        }
        auto last = name;
        for(; !token.is_eod() && !token.is_eof(); token = lexer.advance()) {
            last = token;
        }
        symbol.range = {name.range.begin, last.range.end};

        auto* level = &symbols;
        for(bool descended = true; descended;) {
            descended = false;
            for(auto& candidate: *level) {
                if(candidate.range.begin <= symbol.range.begin &&
                   symbol.range.end <= candidate.range.end) {
                    level = &candidate.children;
                    descended = true;
                    break;
                }
            }
        }
        level->push_back(std::move(symbol));
    }

    /// Xcode's sections, which clangd follows: `#pragma mark - Name` groups
    /// the symbols of its scope after it, up to the scope's next mark or
    /// its end; `#pragma mark Name` is an entry of its own.
    void add_marks() {
        llvm::SmallVector<LexicalInfo::PragmaMark> marks;
        for(const auto& mark: unit.semantics().pragma_marks()) {
            if(!inactive(mark.range.begin)) {
                marks.push_back(mark);
            }
        }

        auto content = unit.main_content();
        DocumentSymbol root{
            .range = {0, static_cast<std::uint32_t>(content.size())},
            .children = std::move(symbols),
        };

        auto child_containing = [](DocumentSymbol& scope,
                                   LocalSourceRange range) -> DocumentSymbol* {
            auto child = llvm::find_if(scope.children, [&](const DocumentSymbol& child) {
                return child.range.contains(range);
            });
            return child == scope.children.end() ? nullptr : &*child;
        };

        for(auto [index, mark]: llvm::enumerate(marks)) {
            auto* scope = &root;
            while(auto* child = child_containing(*scope, mark.range)) {
                scope = child;
            }

            DocumentSymbol symbol{
                .kind = SymbolKind::Directive,
                .range = mark.range,
                .selection_range = mark.range,
            };
            auto text = content.slice(mark.text.begin, mark.text.end).trim();
            auto group = text;
            if(!group.consume_front("-") || (!group.empty() && group.ltrim() == group)) {
                symbol.name = text.empty() ? "(unnamed mark)" : text.str();
                scope->children.push_back(std::move(symbol));
                continue;
            }
            symbol.name = group.empty() ? "(unnamed group)" : group.ltrim().str();

            auto end = scope->range.end;
            for(const auto& next: llvm::ArrayRef(marks).drop_front(index + 1)) {
                if(!scope->range.contains(next.range)) {
                    break;
                }
                if(!child_containing(*scope, next.range)) {
                    end = next.range.begin;
                    break;
                }
            }

            auto& siblings = scope->children;
            auto owned =
                std::ranges::stable_partition(siblings, [&](const DocumentSymbol& sibling) {
                    return sibling.range.begin < mark.range.begin || sibling.range.begin >= end;
                });
            for(auto& child: owned) {
                symbol.range.end = std::max(symbol.range.end, child.range.end);
                symbol.children.push_back(std::move(child));
            }
            siblings.erase(owned.begin(), owned.end());
            siblings.push_back(std::move(symbol));
        }

        symbols = std::move(root.children);
    }

    /// The module or partition the unit defines.
    void add_module(const LexicalInfo::ModuleDeclaration& module) {
        if(module.kind != LexicalInfo::ModuleDeclaration::Kind::Declaration ||
           !unit.defines_module()) {
            return;
        }
        auto name = module.name_range();
        symbols.push_back({
            .name = unit.module_name().str(),
            .kind = SymbolKind::Module,
            .range = name,
            .selection_range = name,
        });
    }

    static bool is_supported(const clang::Decl* decl) {
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
            case clang::Decl::MSProperty:
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
    /// Sorted begin/end offset pairs.
    llvm::ArrayRef<std::uint32_t> inactive_regions;
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

auto to_protocol_symbol(const DocumentSymbol& symbol,
                        const PositionMap& map,
                        PositionEncoding encoding) -> std::optional<protocol::DocumentSymbol> {
    auto range = map.range(symbol.range, encoding);
    auto selection_range = map.range(symbol.selection_range, encoding);
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

    if(symbol.deprecated) {
        result.tags = std::vector{protocol::SymbolTag::Deprecated};
    }

    if(!symbol.children.empty()) {
        std::vector<protocol::DocumentSymbol> children;
        children.reserve(symbol.children.size());
        for(const auto& child: symbol.children) {
            if(auto converted = to_protocol_symbol(child, map, encoding)) {
                children.push_back(std::move(*converted));
            }
        }
        result.children = std::move(children);
    }

    return result;
}

auto outline(CompilationUnitRef unit, llvm::ArrayRef<std::uint32_t> inactive_regions)
    -> std::vector<DocumentSymbol> {
    auto result = Collector(unit, inactive_regions).collect();
    sort_symbols(result);
    return result;
}

}  // namespace

auto document_symbols(CompilationUnitRef unit) -> std::vector<DocumentSymbol> {
    return outline(unit, inactive_regions(unit).regions);
}

auto document_symbols(CompilationUnitRef unit,
                      llvm::ArrayRef<std::uint32_t> inactive_regions,
                      PositionEncoding encoding) -> std::vector<protocol::DocumentSymbol> {
    return document_symbols_to_protocol(outline(unit, inactive_regions),
                                        unit.positions(),
                                        encoding);
}

auto document_symbols_to_protocol(llvm::ArrayRef<DocumentSymbol> symbols,
                                  const PositionMap& map,
                                  PositionEncoding encoding)
    -> std::vector<protocol::DocumentSymbol> {
    std::vector<protocol::DocumentSymbol> result;
    result.reserve(symbols.size());

    for(const auto& symbol: symbols) {
        if(auto converted = to_protocol_symbol(symbol, map, encoding)) {
            result.push_back(std::move(*converted));
        }
    }

    return result;
}

}  // namespace clice::feature
