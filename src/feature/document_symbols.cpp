module clice;

import :compile.compilation;
import :feature.document_symbols;
import :feature.inactive_regions;
import :feature.index_entries;
import :index.query;
import :index.symbol;
import :semantic.decls;
import :semantic.display;
import :semantic.identity;
import :semantic.semantics;
import :semantic.symbol;
import :support.text;
import :syntax.lexical_scan;
import :syntax.source_text;

namespace clice::feature {

auto to_protocol(SymbolKind kind) -> protocol::SymbolKind {
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
        case SymbolKind::Operator: return Operator;
        case SymbolKind::Invalid: return Variable;
    }

    return Variable;
}

bool outline_lists(SymbolKind kind, bool unnamed, bool template_parameter) {
    if(template_parameter) {
        return false;
    }
    switch(kind) {
        case SymbolKind::Namespace:
        case SymbolKind::Class:
        case SymbolKind::Struct:
        case SymbolKind::Union:
        case SymbolKind::Enum: return true;
        case SymbolKind::Type:
        case SymbolKind::Concept:
        case SymbolKind::Field:
        case SymbolKind::EnumMember:
        case SymbolKind::Function:
        case SymbolKind::Method:
        case SymbolKind::Operator:
        case SymbolKind::Variable: return !unnamed;
        default: return false;
    }
}

namespace {

std::string spelling(llvm::StringRef content, const Token& token) {
    return unspliced(token.text(content));
}

bool is_word(const Token& token) {
    return token.is_identifier() || token.kind == clang::tok::numeric_constant;
}

/// Tokens as one text: a space between two words and after a comma, none
/// elsewhere.
std::string joined(llvm::StringRef content, llvm::ArrayRef<Token> tokens) {
    std::string text;
    const Token* previous = nullptr;
    for(const auto& token: tokens) {
        if(previous &&
           ((is_word(*previous) && is_word(token)) || previous->kind == clang::tok::comma)) {
            text += ' ';
        }
        text += spelling(content, token);
        previous = &token;
    }
    return text;
}

/// Whether the text at `selection` spells `name`, whitespace aside.
bool spells(llvm::StringRef content, LocalSourceRange selection, llvm::StringRef name) {
    auto squeezed = [](llvm::StringRef text) {
        std::string result;
        for(char c: text) {
            if(!clang::isWhitespace(c) && c != '\\') {
                result += c;
            }
        }
        return result;
    };
    return squeezed(content.slice(selection.begin, selection.end)) == squeezed(name);
}

/// `name` with the nested name qualifier written before it and the template
/// argument list written after it, `selection` spelling the name in the
/// declaration `range`: `Box<T*>::reset`, `Box<void>`.
std::string written_name(SourceText& source,
                         LocalSourceRange range,
                         LocalSourceRange selection,
                         llvm::StringRef name) {
    auto content = source.text();
    auto tokens = source.lexical().code_from(range.begin);
    auto token = tokens.begin();
    llvm::SmallVector<Token> before;
    for(; token != tokens.end() && token->range.begin < selection.begin; ++token) {
        if(token->range.begin >= range.begin) {
            before.push_back(*token);
        }
    }
    for(; token != tokens.end() && token->range.begin < selection.end; ++token) {}

    llvm::SmallVector<Token> arguments;
    if(token != tokens.end() && token->kind == clang::tok::less) {
        std::int32_t angles = 0;
        std::int32_t brackets = 0;
        for(; token != tokens.end() && !token->is_eod(); ++token) {
            arguments.push_back(*token);
            switch(token->kind) {
                case clang::tok::l_paren:
                case clang::tok::l_square:
                case clang::tok::l_brace: brackets += 1; break;
                case clang::tok::r_paren:
                case clang::tok::r_square:
                case clang::tok::r_brace: brackets -= 1; break;
                case clang::tok::less: angles += brackets == 0 ? 1 : 0; break;
                case clang::tok::greater: angles -= brackets == 0 ? 1 : 0; break;
                case clang::tok::greatergreater: angles -= brackets == 0 ? 2 : 0; break;
                default: break;
            }
            if(angles <= 0) {
                break;
            }
        }
        if(angles != 0) {
            arguments.clear();
        }
    }

    // Back from the name over `component ::` pairs, a component a name with
    // its template arguments: `ns::Box<T>::`.
    auto named = [&](const Token& token) {
        return source.kind(token) == clang::tok::identifier;
    };
    auto start = before.size();
    while(start > 1 && before[start - 1].kind == clang::tok::coloncolon) {
        auto component = start - 2;
        if(before[component].kind == clang::tok::greater ||
           before[component].kind == clang::tok::greatergreater) {
            std::int32_t depth = 0;
            auto at = component + 1;
            for(; at > 0; at -= 1) {
                auto kind = before[at - 1].kind;
                depth += kind == clang::tok::greater          ? 1
                         : kind == clang::tok::greatergreater ? 2
                         : kind == clang::tok::less           ? -1
                                                              : 0;
                if(depth <= 0) {
                    break;
                }
            }
            if(depth != 0 || at < 2) {
                break;
            }
            component = at - 2;
        }
        if(!named(before[component])) {
            break;
        }
        start = component;
        if(start > 0 && before[start - 1].is_identifier() &&
           before[start - 1].text(content) == "template") {
            start -= 1;
        }
    }

    auto qualifier = joined(content, llvm::ArrayRef(before).drop_front(start));
    return qualifier + name.str() + joined(content, arguments);
}

/// The outline entry of the `#define` directive `directive`: its name, as
/// detail the parameters a function-like macro takes, and the range from
/// the name through the last token.
std::optional<OutlineEntry> macro_entry(llvm::StringRef content,
                                        const LexicalInfo& lexical,
                                        const LexicalInfo::Directive& directive) {
    auto words = lexical.words(directive);
    auto definition = macro_definition(words);
    if(!definition) {
        return std::nullopt;
    }

    auto& name = definition->name;
    OutlineEntry entry{
        .name = spelling(content, name),
        .kind = SymbolKind::Macro,
        .range = {name.range.begin, words.back().range.end},
        .selection_range = name.range,
        .definition = true,
    };
    if(auto parameters = definition->parameters; !parameters.empty()) {
        if(parameters.back().kind == clang::tok::r_paren) {
            parameters = parameters.drop_back();
        }
        std::string detail = "(";
        for(const auto& token: parameters.drop_front()) {
            detail += token.kind == clang::tok::comma ? ", " : spelling(content, token);
        }
        detail += ')';
        entry.detail = std::move(detail);
    }
    return entry;
}

/// The macro invocation whose name `name` is: as written, the arguments
/// cut after 80 characters, and the range it spans.
std::pair<std::string, LocalSourceRange> invocation(llvm::StringRef content,
                                                    const LexicalInfo& lexical,
                                                    LocalSourceRange name) {
    auto tokens = lexical.code_from(name.begin);
    auto token = tokens.begin();
    auto text = spelling(content, *token);
    LocalSourceRange range = name;
    ++token;
    if(token == tokens.end() || token->kind != clang::tok::l_paren) {
        return {std::move(text), range};
    }
    llvm::SmallVector<Token> arguments;
    std::int32_t depth = 0;
    for(; token != tokens.end(); ++token) {
        arguments.push_back(*token);
        depth += token->kind == clang::tok::l_paren   ? 1
                 : token->kind == clang::tok::r_paren ? -1
                                                      : 0;
        if(depth == 0) {
            break;
        }
    }
    if(depth != 0) {
        return {std::move(text), range};
    }
    range.end = token->range.end;
    constexpr std::size_t limit = 80;
    auto written = joined(content, arguments);
    if(written.size() > limit) {
        // At a character's first byte, not inside a UTF-8 sequence.
        auto cut = limit;
        while(cut > 0 && (static_cast<unsigned char>(written[cut]) & 0xC0) == 0x80) {
            cut -= 1;
        }
        written.resize(cut);
        written += "...)";
    }
    return {text + written, range};
}

/// The name a module declaration writes: `app.core:detail`.
std::string module_name(llvm::StringRef content, const LexicalInfo::ModuleDeclaration& module) {
    auto dotted = [&](llvm::ArrayRef<LocalSourceRange> parts) {
        llvm::SmallVector<std::string> names;
        for(auto part: parts) {
            names.push_back(unspliced(content.slice(part.begin, part.end)));
        }
        return llvm::join(names, ".");
    };
    auto name = dotted(module.name_parts);
    if(!module.partition_parts.empty()) {
        name += ":" + dotted(module.partition_parts);
    }
    return name;
}

/// Xcode's sections, which clangd follows: `#pragma mark - Name` groups
/// the symbols of its scope after it, up to the scope's next mark or
/// its end; `#pragma mark Name` is an entry of its own.
void add_marks(std::vector<DocumentSymbol>& symbols,
               const LexicalInfo& lexical,
               llvm::ArrayRef<LexicalInfo::Directive> marks,
               llvm::StringRef content) {
    DocumentSymbol root{
        .range = {0, static_cast<std::uint32_t>(content.size())},
        .children = std::move(symbols),
    };

    auto child_containing = [](DocumentSymbol& scope, LocalSourceRange range) -> DocumentSymbol* {
        auto child = ranges::find_if(scope.children, [&](const DocumentSymbol& child) {
            return child.range.contains(range);
        });
        return child == scope.children.end() ? nullptr : &*child;
    };

    for(auto [index, mark]: views::enumerate(marks)) {
        auto* scope = &root;
        while(auto* child = child_containing(*scope, mark.range)) {
            scope = child;
        }

        DocumentSymbol symbol{
            .kind = SymbolKind::Directive,
            .range = mark.range,
            .selection_range = mark.range,
        };
        // What follows `mark` on the line, unlexed as clang reads it.
        auto text = content.slice(lexical.words(mark)[1].range.end, mark.range.end).trim();
        auto group = text;
        if(!group.consume_front("-") || (!group.empty() && group.ltrim() == group)) {
            symbol.name = text.empty() ? "(unnamed mark)" : text.str();
            scope->children.push_back(std::move(symbol));
            continue;
        }
        symbol.name = group.empty() ? "(unnamed group)" : group.ltrim().str();

        auto end = scope->range.end;
        for(const auto& next: marks.drop_front(static_cast<std::size_t>(index) + 1)) {
            if(!scope->range.contains(next.range)) {
                break;
            }
            if(!child_containing(*scope, next.range)) {
                end = next.range.begin;
                break;
            }
        }

        auto& siblings = scope->children;
        auto owned = ranges::stable_partition(siblings, [&](const DocumentSymbol& sibling) {
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

/// Sibling order: by position, a shorter range first, then by name and
/// kind — the members of one macro invocation share their range.
void sort_symbols(std::vector<DocumentSymbol>& symbols) {
    ranges::stable_sort(symbols, {}, [](const DocumentSymbol& symbol) {
        return std::tuple(symbol.range.begin,
                          symbol.range.end,
                          std::string_view(symbol.name),
                          symbol.kind.value());
    });
    for(auto& symbol: symbols) {
        sort_symbols(symbol.children);
    }
}

bool is_record(SymbolKind kind) {
    return kind == SymbolKind::Class || kind == SymbolKind::Struct || kind == SymbolKind::Union;
}

bool is_tag(SymbolKind kind) {
    return is_record(kind) || kind == SymbolKind::Enum;
}

/// What a function or a variable declares lists under it, as the locals
/// option decides.
bool holds_locals(SymbolKind kind) {
    return kind == SymbolKind::Function || kind == SymbolKind::Method ||
           kind == SymbolKind::Operator || kind == SymbolKind::Variable ||
           kind == SymbolKind::Field;
}

/// An entry while the outline nests it.
struct Node {
    OutlineEntry entry;

    /// Where the entry is named for nesting: an entry holds those written
    /// after it there. A struct a typedef names is named at the struct.
    LocalSourceRange anchor;

    /// The macro invocation node of an entry it makes; its members list
    /// flat under it.
    std::optional<std::uint32_t> group;
    bool is_group = false;

    /// Of a macro invocation node, the invocation's text.
    LocalSourceRange invocation;

    std::vector<std::uint32_t> children;
};

/// Whether `outer` holds `inner` in the outline.
bool holds(llvm::ArrayRef<Node> nodes, std::uint32_t outer_index, const Node& inner) {
    const auto& outer = nodes[outer_index];
    if(!outer.entry.range.contains(inner.entry.range)) {
        return false;
    }
    if(inner.group == outer_index) {
        return true;
    }
    // What an invocation's arguments name lists beside what its body makes.
    if(outer.group &&
       (outer.group == inner.group || nodes[*outer.group].invocation.contains(inner.anchor))) {
        return false;
    }
    return outer.anchor.end <= inner.anchor.begin;
}

/// Whether a typedef names the tag written before its name, not a
/// pointer, reference or array of it (`} *Ref;`, `} Point, *Ref;`,
/// `} Grid[3];`).
bool names_tag(const LexicalInfo& lexical, const OutlineEntry& tag, const OutlineEntry& alias) {
    if(alias.range.end > alias.selection_range.end) {
        return false;
    }
    for(const auto& token: lexical.code_from(tag.range.end)) {
        if(token.range.begin >= alias.selection_range.begin) {
            break;
        }
        if(token.range.begin < tag.range.end) {
            continue;
        }
        switch(token.kind) {
            case clang::tok::star:
            case clang::tok::amp:
            case clang::tok::ampamp:
            case clang::tok::caret:
            case clang::tok::comma: return false;
            default: break;
        }
    }
    return true;
}

/// `typedef struct { ... } Point;`: the struct the typedef declares and
/// names, unnamed or named alike, lists once, as the typedef's name.
void merge_typedefs(std::vector<Node>& nodes, const LexicalInfo& lexical) {
    std::vector<std::uint32_t> tags;
    for(auto [index, node]: views::enumerate(nodes)) {
        if(is_tag(node.entry.kind)) {
            tags.push_back(static_cast<std::uint32_t>(index));
        }
    }
    ranges::sort(tags, {}, [&](std::uint32_t index) { return nodes[index].entry.range.begin; });

    std::vector<bool> merged(nodes.size(), false);
    for(auto& alias: nodes) {
        if(alias.entry.kind != SymbolKind::Type) {
            continue;
        }
        auto first = ranges::upper_bound(tags, alias.entry.range.begin, {}, [&](auto index) {
            return nodes[index].entry.range.begin;
        });
        for(auto it = first; it != tags.end(); ++it) {
            auto& tag = nodes[*it];
            if(tag.entry.range.begin >= alias.entry.selection_range.begin) {
                break;
            }
            if(merged[*it] || tag.entry.range.end > alias.entry.selection_range.begin ||
               (!tag.entry.unnamed && tag.entry.name != alias.entry.name) ||
               !names_tag(lexical, tag.entry, alias.entry)) {
                continue;
            }
            merged[*it] = true;
            alias.entry.kind = tag.entry.kind;
            alias.entry.detail = std::move(tag.entry.detail);
            alias.entry.deprecated |= tag.entry.deprecated;
            alias.entry.entity = tag.entry.entity;
            alias.entry.parent = tag.entry.parent;
            alias.entry.definition = tag.entry.definition;
            alias.anchor = tag.anchor;
            break;
        }
    }
    std::uint32_t index = 0;
    std::erase_if(nodes, [&](const Node&) {
        bool erased = merged[index];
        index += 1;
        return erased;
    });
}

}  // namespace

auto outline(std::vector<OutlineEntry> entries,
             SourceText& source,
             llvm::ArrayRef<LexicalInfo::Directive> macros,
             llvm::ArrayRef<LexicalInfo::ModuleDeclaration> modules,
             llvm::ArrayRef<LexicalInfo::Directive> marks,
             const DocumentSymbolsOptions& options) -> std::vector<DocumentSymbol> {
    auto content = source.text();
    const auto& lexical = source.lexical();
    for(auto& entry: entries) {
        if(!entry.from_macro && !entry.unnamed &&
           spells(content, entry.selection_range, entry.name)) {
            entry.name = written_name(source, entry.range, entry.selection_range, entry.name);
        }
    }
    if(options.macros) {
        auto guard = lexical.include_guard ? lexical.include_guard->name : LocalSourceRange();
        for(const auto& directive: macros) {
            if(auto entry = macro_entry(content, lexical, directive);
               entry && entry->selection_range != guard) {
                entries.push_back(std::move(*entry));
            }
        }
    }
    for(const auto& module: modules) {
        if(module.kind == LexicalInfo::ModuleDeclaration::Kind::Declaration) {
            auto name = module.name_range();
            entries.push_back({
                .name = module_name(content, module),
                .kind = SymbolKind::Module,
                .range = name,
                .selection_range = name,
                .definition = true,
            });
        }
    }

    // LSP wants the selection inside the range; a macro invocation's
    // expansion range may be narrower than the name its argument spells.
    for(auto& entry: entries) {
        entry.range.begin = std::min(entry.range.begin, entry.selection_range.begin);
        entry.range.end = std::max(entry.range.end, entry.selection_range.end);
    }

    // A merged index holds a declaration once per context it compiles in,
    // and an explicit instantiation's local once more: a written name is
    // one entry, whatever entities the index gives it there, with the
    // widest extent among them.
    auto key = [](const OutlineEntry& entry) {
        return std::tuple(entry.selection_range.begin,
                          entry.selection_range.end,
                          entry.from_macro ? entry.entity : 0,
                          std::string_view(entry.name),
                          entry.kind.value());
    };
    auto width = [](const OutlineEntry& entry) {
        return entry.range.end - entry.range.begin;
    };
    ranges::stable_sort(entries, [&](const OutlineEntry& lhs, const OutlineEntry& rhs) {
        return std::tuple(key(lhs), width(rhs)) < std::tuple(key(rhs), width(lhs));
    });
    auto duplicates = ranges::unique(entries, {}, key);
    entries.erase(duplicates.begin(), duplicates.end());

    std::vector<Node> nodes;
    nodes.reserve(entries.size());
    for(auto& entry: entries) {
        auto anchor = entry.selection_range;
        nodes.push_back({.entry = std::move(entry), .anchor = anchor});
    }
    merge_typedefs(nodes, lexical);

    // The members a macro invocation makes list under it.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> groups;
    auto count = static_cast<std::uint32_t>(nodes.size());
    for(std::uint32_t index = 0; index < count; index += 1) {
        if(!nodes[index].entry.from_macro) {
            continue;
        }
        auto name = nodes[index].entry.selection_range;
        auto [group, created] = groups.try_emplace({name.begin, name.end}, nodes.size());
        if(created) {
            auto [text, range] = invocation(content, lexical, name);
            nodes.push_back({
                .entry = {.name = std::move(text),
                          .kind = SymbolKind::Macro,
                          .range = range,
                          .selection_range = name,
                          .definition = true},
                .anchor = name,
                .is_group = true,
                .invocation = range,
            });
        }
        auto& node = nodes[index];
        auto& invoked = nodes[group->second];
        invoked.entry.range.begin = std::min(invoked.entry.range.begin, node.entry.range.begin);
        invoked.entry.range.end = std::max(invoked.entry.range.end, node.entry.range.end);
        node.group = group->second;
    }

    std::vector<std::uint32_t> order(nodes.size());
    std::iota(order.begin(), order.end(), 0U);
    ranges::stable_sort(order, [&](std::uint32_t lhs, std::uint32_t rhs) {
        const auto& left = nodes[lhs];
        const auto& right = nodes[rhs];
        if(left.entry.range.begin != right.entry.range.begin) {
            return left.entry.range.begin < right.entry.range.begin;
        }
        if(left.entry.range.end != right.entry.range.end) {
            return left.entry.range.end > right.entry.range.end;
        }
        if(left.is_group != right.is_group) {
            return left.is_group;
        }
        return std::tie(left.anchor.begin, left.entry.name) <
               std::tie(right.anchor.begin, right.entry.name);
    });

    // Each entry under the innermost open one holding it. Open entries
    // that end before it hold nothing after it; the others stay open, as
    // the members of one invocation overlap without holding each other.
    std::vector<std::uint32_t> roots;
    std::vector<std::uint32_t> open;
    for(auto index: order) {
        auto& node = nodes[index];
        auto holder = std::find_if(open.rbegin(), open.rend(), [&](std::uint32_t candidate) {
            return holds(nodes, candidate, node);
        });
        auto* parent = holder == open.rend() ? nullptr : &nodes[*holder];
        std::erase_if(open, [&](std::uint32_t candidate) {
            return nodes[candidate].entry.range.end <= node.entry.range.begin;
        });
        // A friend declaration names an entity of the enclosing scope; only
        // a hidden friend's definition lives nowhere else in the outline.
        if(parent && is_record(parent->entry.kind) && !node.entry.definition &&
           node.entry.parent != parent->entry.entity) {
            continue;
        }
        (parent ? parent->children : roots).push_back(index);
        open.push_back(index);
    }

    auto build = [&](this auto& self,
                     llvm::ArrayRef<std::uint32_t> indices) -> std::vector<DocumentSymbol> {
        std::vector<DocumentSymbol> symbols;
        for(auto index: indices) {
            auto& node = nodes[index];
            // FIXME: a local a macro body declares in a function the same macro defines
            // (`#define MAKE void f() { int local = 0; }`) lists anyway: macro-made members
            // list flat under the invocation, so the local is no child of its function.
            bool locals = options.locals || !holds_locals(node.entry.kind);
            symbols.push_back({
                .name = std::move(node.entry.name),
                .detail = std::move(node.entry.detail),
                .kind = node.entry.kind,
                .range = node.entry.range,
                .selection_range = node.entry.selection_range,
                .deprecated = node.entry.deprecated,
                .children = locals ? self(node.children) : std::vector<DocumentSymbol>{},
                .entity = node.entry.entity,
            });
        }
        return symbols;
    };
    auto symbols = build(roots);
    add_marks(symbols, lexical, marks, content);
    sort_symbols(symbols);
    return symbols;
}

namespace {

/// The declarations of the unit's focus file the outline lists, from the
/// unit's cached Semantics node table — the DFS pre-order record of the
/// file's written AST — instead of another RecursiveASTVisitor over the TU.
class Builder {
public:
    explicit Builder(CompilationUnit& unit) :
        unit(unit), entities(unit.get<EntityTable>()), readable(unit.context(), {}) {}

    auto build() -> std::vector<OutlineEntry> {
        auto nodes = unit.get<Semantics>().node_entries();
        std::uint32_t index = 0;
        while(index < nodes.size()) {
            const Semantics::Node& entry = nodes[index];
            if(!entry.node.is_ast()) {
                // The preprocessor segment follows the AST segment.
                break;
            }

            // Implicit instantiations (and anything below an instantiation
            // head) are located at the pattern, not here.
            if(entry.flags.in_instantiation) {
                index = entry.subtree_end;
                continue;
            }

            if(entry.node.kind() == SemanticNode::Kind::Decl &&
               !visit(entry.node.get<clang::Decl>())) {
                index = entry.subtree_end;
                continue;
            }

            index += 1;
        }
        return std::move(entries);
    }

private:
    /// Returns false when the decl's whole subtree should be skipped.
    bool visit(const clang::Decl* decl) {
        // An explicit instantiation directive outlines as a childless entry
        // of its specialization: the instantiated members sit at the pattern.
        if(const auto* directive = llvm::dyn_cast<clang::ExplicitInstantiationDecl>(decl)) {
            add(*directive->getSpecialization(),
                directive->getNameLoc(),
                directive->getSourceRange(),
                false);
            return false;
        }

        const auto* named = llvm::dyn_cast<clang::NamedDecl>(decl);
        if(!named || !outline_lists(SymbolKind::from(named),
                                    display::symbol_name(named).unnamed,
                                    named->isTemplateParameter())) {
            return true;
        }

        // A function's declaration name may span several tokens
        // (`~Widget`, `operator bool`, `operator==`); NameInfo covers them
        // all, while getLocation() is only the first one.
        clang::SourceRange name_range = named->getLocation();
        bool definition = true;
        if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(named)) {
            name_range = function->getNameInfo().getSourceRange();
            definition = function->isThisDeclarationADefinition();
        } else if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(named)) {
            definition = tag->isThisDeclarationADefinition();
        }

        // A fragment's members outline at its top level.
        return add(*named, name_range, decls::written_range(named), definition) ||
               unit.encloses_focus_file(decl);
    }

    /// Appends the entry; false when a range falls outside the focus file.
    bool add(const clang::NamedDecl& named,
             clang::SourceRange name_range,
             clang::SourceRange full_range,
             bool definition) {
        // Names spelled inside a macro argument (`DEFINE(name)`) select the
        // written spelling; names spelled in the macro body keep the
        // invocation site, and a name a macro spells part of its first
        // token, as the index records them.
        auto& SM = unit.context().getSourceManager();
        auto location = SM.getFileLoc(name_range.getBegin());
        bool from_macro = location != SM.getSpellingLoc(name_range.getBegin());
        auto end = SM.getFileLoc(name_range.getEnd());
        bool whole = !from_macro && end == SM.getSpellingLoc(name_range.getEnd()) &&
                     SM.getFileID(end) == SM.getFileID(location) &&
                     SM.getFileOffset(location) <= SM.getFileOffset(end);
        name_range = clang::SourceRange(location, whole ? end : location);

        auto focus = unit.focus_file();
        auto selection_range = focus.written_range(name_range);
        auto range = focus.produced_range(full_range);
        if(!selection_range || !range || !selection_range->valid() || !range->valid()) {
            return false;
        }

        // The index describes an entity by its first declaration.
        const auto* entity = decls::normalize(&named);
        auto symbol_name = display::symbol_name(entity);
        entries.push_back({
            .name = std::move(symbol_name.text),
            .detail = detail(named),
            .kind = SymbolKind::from(entity),
            .range = *range,
            .selection_range = *selection_range,
            .deprecated = named.getMostRecentDecl()->getAvailability() == clang::AR_Deprecated,
            .entity = entities.entity(entity),
            .parent = entities.parent(&named),
            .definition = definition,
            .unnamed = symbol_name.unnamed,
            .from_macro = from_macro,
        });
        return true;
    }

    /// What the outline shows beside a name: a function's signature with
    /// `static`, `virtual` and `= 0`, a variable's type, a class's kind and
    /// bases.
    auto detail(const clang::NamedDecl& decl) -> std::string {
        auto& context = unit.context();
        display::Options options = {
            .suppress_unwritten_scope = true,
            .polish_for_declaration = true,
            .suppress_ctor_template_args = true,
            .clean_uglified = true,
            .readable = &readable,
            .scope = decl.getDeclContext(),
        };
        auto type = [&](clang::QualType type) {
            return display::type(context, type, options).text;
        };
        // `int (int x)`, `Ops &(int)`, as a function type prints.
        auto signature = [&](const clang::FunctionDecl& function) {
            auto returned = type(function.getReturnType());
            auto spaced = !llvm::StringRef(returned).ends_with("*") &&
                          !llvm::StringRef(returned).ends_with("&");
            return returned + (spaced ? " " : "") + display::call_signature(function, options);
        };

        llvm::SmallVector<std::string> parts;
        if(decl.getDescribedTemplateParams()) {
            parts.push_back("template");
        }

        if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&decl)) {
            const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function);
            if(method && method->isStatic()) {
                parts.push_back("static");
            }
            if(method && method->isVirtual()) {
                parts.push_back("virtual");
            }
            if(llvm::isa<clang::CXXConstructorDecl>(function)) {
                parts.push_back(display::call_signature(*function, options));
            } else if(!llvm::isa<clang::CXXDestructorDecl>(function)) {
                parts.push_back(signature(*function));
            }
            if(function->isPureVirtual()) {
                parts.push_back("= 0");
            }
        } else if(const auto* value = llvm::dyn_cast<clang::ValueDecl>(&decl);
                  // A binding of a declaration in error has no type.
                  value && !value->getType().isNull()) {
            if(const auto* var = llvm::dyn_cast<clang::VarDecl>(value);
               var && var->isStaticDataMember()) {
                parts.push_back("static");
            }
            // A lambda shows what calling it takes.
            const auto* closure = value->getType().getNonReferenceType()->getAsCXXRecordDecl();
            if(closure && closure->isLambda() && closure->getLambdaCallOperator()) {
                const auto* call = closure->getLambdaCallOperator();
                auto text = signature(*call);
                text.resize(text.size() - display::method_qualifiers(*call).size());
                parts.push_back(std::move(text));
            } else {
                parts.push_back(type(value->getType()));
            }
        } else if(const auto* tag = llvm::dyn_cast<clang::TagDecl>(&decl)) {
            std::string text = tag->getKindName().str();
            const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(tag);
            if(record && record->isThisDeclarationADefinition() && record->getNumBases() != 0) {
                llvm::SmallVector<std::string> bases;
                for(const auto& base: record->bases()) {
                    std::string written;
                    if(base.isVirtual()) {
                        written += "virtual ";
                    }
                    if(auto access = base.getAccessSpecifierAsWritten(); access != clang::AS_none) {
                        written += clang::getAccessSpelling(access);
                        written += " ";
                    }
                    written += type(base.getTypeSourceInfo()->getType());
                    if(base.isPackExpansion()) {
                        written += "...";
                    }
                    bases.push_back(std::move(written));
                }
                text += " : " + llvm::join(bases, ", ");
            }
            parts.push_back(std::move(text));
        } else if(llvm::isa<clang::TypedefNameDecl>(&decl)) {
            parts.push_back("type alias");
        } else if(llvm::isa<clang::NamespaceAliasDecl>(&decl)) {
            parts.push_back("namespace alias");
        } else if(llvm::isa<clang::ConceptDecl>(&decl)) {
            parts.push_back("concept");
        }
        return llvm::join(parts, " ");
    }

    CompilationUnit& unit;
    EntityTable& entities;
    display::ReadableTypes readable;
    std::vector<OutlineEntry> entries;
};

/// The protocol's kind of a symbol, as the client takes kinds: clangd's
/// stand-ins for one it lacks.
auto client_kind(SymbolKind kind, const SymbolClient& client) -> protocol::SymbolKind {
    auto result = to_protocol(kind);
    if(client.all_kinds || result <= protocol::SymbolKind::Array) {
        return result;
    }
    switch(result) {
        case protocol::SymbolKind::Struct: return protocol::SymbolKind::Class;
        case protocol::SymbolKind::EnumMember: return protocol::SymbolKind::Enum;
        default: return protocol::SymbolKind::String;
    }
}

auto to_protocol_symbol(const DocumentSymbol& symbol,
                        const PositionMap& map,
                        PositionEncoding encoding,
                        const SymbolClient& client) -> std::optional<protocol::DocumentSymbol> {
    auto range = map.range(symbol.range, encoding);
    auto selection_range = map.range(symbol.selection_range, encoding);
    if(!range || !selection_range)
        return std::nullopt;

    protocol::DocumentSymbol result{
        .name = symbol.name,
        .kind = client_kind(symbol.kind, client),
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
            if(auto converted = to_protocol_symbol(child, map, encoding, client)) {
                children.push_back(std::move(*converted));
            }
        }
        result.children = std::move(children);
    }

    return result;
}

}  // namespace

auto document_symbols(CompilationUnit& unit, const DocumentSymbolsOptions& options)
    -> std::vector<DocumentSymbol> {
    return document_symbols(unit, inactive_regions(unit).regions, options);
}

auto document_symbols(CompilationUnit& unit,
                      llvm::ArrayRef<std::uint32_t> inactive_regions,
                      const DocumentSymbolsOptions& options) -> std::vector<DocumentSymbol> {
    auto inactive = [&](std::uint32_t offset) {
        auto bounds = ranges::upper_bound(inactive_regions, offset) - inactive_regions.begin();
        return bounds % 2 == 1;
    };
    auto& semantics = unit.get<Semantics>();
    auto& source = unit.focus_file().text();
    const auto& lexical = source.lexical();
    std::vector<LexicalInfo::Directive> macros;
    std::vector<LexicalInfo::Directive> marks;
    for(const auto& directive: lexical.directives) {
        if(inactive(directive.range.begin)) {
            continue;
        }
        if(directive.kind == LexicalInfo::Directive::Kind::Define) {
            macros.push_back(directive);
        } else if(directive.kind == LexicalInfo::Directive::Kind::PragmaMark) {
            marks.push_back(directive);
        }
    }
    return outline(Builder(unit).build(),
                   source,
                   macros,
                   semantics.module_declarations(),
                   marks,
                   options);
}

auto to_protocol(llvm::ArrayRef<DocumentSymbol> symbols,
                 const PositionMap& map,
                 PositionEncoding encoding,
                 const SymbolClient& client) -> std::vector<protocol::DocumentSymbol> {
    std::vector<protocol::DocumentSymbol> result;
    result.reserve(symbols.size());

    for(const auto& symbol: symbols) {
        if(auto converted = to_protocol_symbol(symbol, map, encoding, client)) {
            result.push_back(std::move(*converted));
        }
    }

    return result;
}

auto to_symbol_information(llvm::ArrayRef<DocumentSymbol> symbols,
                           const PositionMap& map,
                           PositionEncoding encoding,
                           const SymbolClient& client,
                           llvm::StringRef uri) -> std::vector<protocol::SymbolInformation> {
    std::vector<protocol::SymbolInformation> result;
    auto flatten = [&](this auto& self,
                       llvm::ArrayRef<DocumentSymbol> level,
                       const DocumentSymbol* container) -> void {
        for(const auto& symbol: level) {
            auto range = map.range(symbol.range, encoding);
            if(!range) {
                continue;
            }
            protocol::SymbolInformation information{
                .name = symbol.name,
                .kind = client_kind(symbol.kind, client),
                .location = {.uri = uri.str(), .range = *range},
            };
            if(symbol.deprecated) {
                information.tags = std::vector{protocol::SymbolTag::Deprecated};
            }
            if(container) {
                information.container_name = container->name;
            }
            result.push_back(std::move(information));
            self(symbol.children, &symbol);
        }
    };
    flatten(symbols, nullptr);
    return result;
}

auto document_symbols(SourceText& source,
                      const IndexEntries& entries,
                      IndexSymbolResolver resolve,
                      const DocumentSymbolsOptions& options) -> std::vector<DocumentSymbol> {
    // A name the text writes is where an occurrence of its symbol starts.
    llvm::DenseSet<std::pair<std::uint32_t, index::SymbolHash>> written;
    for(const auto& occurrence: entries.occurrences) {
        written.insert({occurrence.range.begin, occurrence.target});
    }

    std::vector<OutlineEntry> declarations;
    std::vector<std::uint32_t> macro_names;
    llvm::DenseSet<std::uint32_t> module_names;
    auto add = [&](const IndexDecl& decl) {
        if(!decl.extent.valid() || decl.extent.begin >= decl.extent.end) {
            return;
        }
        auto info = resolve(decl.symbol);
        if(!info) {
            return;
        }
        if(info->kind == SymbolKind::Macro) {
            macro_names.push_back(decl.range.begin);
            return;
        }
        if(info->kind == SymbolKind::Module) {
            module_names.insert(decl.range.begin);
            return;
        }
        auto flag = [&](index::SymbolFlags bit) {
            return index::has_flag(info->flags, bit);
        };
        bool unnamed = flag(index::SymbolFlags::Unnamed);
        if(!outline_lists(info->kind, unnamed, flag(index::SymbolFlags::TemplateParameter))) {
            return;
        }
        declarations.push_back({
            .name = std::move(info->name),
            .kind = info->kind,
            .range = decl.extent,
            .selection_range = decl.range,
            .deprecated = flag(index::SymbolFlags::Deprecated),
            .entity = decl.symbol,
            .parent = info->parent,
            .definition = decl.definition,
            .unnamed = unnamed,
            .from_macro = !written.contains({decl.range.begin, decl.symbol}),
        });
    };
    for(const auto& decl: entries.decls) {
        add(decl);
    }
    for(const auto& decl: entries.instantiations) {
        add(decl);
    }

    // The `#define`s the preprocessor read, the ones whose macro the index
    // knows.
    ranges::sort(macro_names);
    const auto& lexical = source.lexical();
    std::vector<LexicalInfo::Directive> macros;
    std::vector<LexicalInfo::Directive> marks;
    for(const auto& directive: lexical.directives) {
        // FIXME: a mark inside an inactive branch (`#if 0`) lists too and sections
        // the declarations after it: the index stores no inactive regions.
        if(directive.kind == LexicalInfo::Directive::Kind::PragmaMark) {
            marks.push_back(directive);
        }
        if(directive.kind != LexicalInfo::Directive::Kind::Define) {
            continue;
        }
        auto name = ranges::lower_bound(macro_names, directive.range.begin);
        if(name != macro_names.end() && *name < directive.range.end) {
            macros.push_back(directive);
        }
    }
    // The module declarations the compile took for one: a raw scan also
    // reads C code such as `module AP_MODULE_DECLARE_DATA example = {`.
    std::vector<LexicalInfo::ModuleDeclaration> modules;
    for(const auto& module: lexical.modules) {
        if(module.kind == LexicalInfo::ModuleDeclaration::Kind::Declaration &&
           module_names.contains(module.name_range().begin)) {
            modules.push_back(module);
        }
    }
    return outline(std::move(declarations), source, macros, modules, marks, options);
}

auto document_symbols(SourceText& source,
                      const index::IndexQuery& query,
                      const index::IndexSource& index,
                      const DocumentSymbolsOptions& options) -> std::vector<DocumentSymbol> {
    return document_symbols(
        source,
        extract_index_entries(*index.shard),
        [&](index::SymbolHash hash) { return query.symbol_info(hash, index.file); },
        options);
}

}  // namespace clice::feature
