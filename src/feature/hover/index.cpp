module clice;

import :feature.document_links;
import :feature.documentation;
import :feature.hover.card;
import :feature.hover;
import :index.query;
import :index.query.site;
import :index.symbol;
import :index.query.text;
import :semantic.comment;
import :semantic.symbol;
import :support.text;
import :syntax.dependency_graph;
import :syntax.lexer;
import :syntax.lexical_scan;
import :vfs.file_table;

namespace clice::feature {

/// The text of a definition before the body its extent ends with, or
/// nullopt when the extent ends otherwise (`= default`, an initializer).
static std::optional<llvm::StringRef> before_body(const std::string& text) {
    // FIXME: a function-try-block's last brace group is its last handler:
    // the card keeps the body and the `catch (...)` heads.
    auto braces = code_braces(lexical_scan(text, dialect_of()));
    auto written = llvm::StringRef(text).rtrim();
    auto body = last_brace_group(braces, {0, static_cast<std::uint32_t>(written.size())});
    if(!body || body->end != written.size()) {
        return std::nullopt;
    }
    return written.take_front(body->begin).rtrim();
}

/// An Objective-C container's text from its keyword on, past the
/// attributes written before it: `@interface Shape : Object ... @end`.
static llvm::StringRef objc_container(llvm::StringRef text) {
    return text.substr(text.find('@'));
}

/// The head of an Objective-C container, which closes with `@end`: the
/// attributes before its keyword, and the keyword's line up to its ivars,
/// `@interface Shape : Object <Drawable>`.
static std::optional<std::string> objc_head(llvm::StringRef text) {
    if(!text.rtrim().ends_with("@end")) {
        return std::nullopt;
    }
    auto container = objc_container(text);
    auto attributes = text.drop_back(container.size()).rtrim();
    auto head = container.take_until([](char c) { return c == '{' || c == '\n'; }).rtrim();
    return attributes.empty() ? head.str() : std::format("{}\n{}", attributes, head);
}

auto hover_info(const index::SymbolInfo& info,
                llvm::ArrayRef<index::SymbolInfo> scopes,
                llvm::StringRef definition_text,
                Documentation documentation) -> HoverInfo {
    HoverInfo hover;
    hover.name = info.display_name();
    hover.kind = info.kind;
    /// The parent chain skips anonymous namespaces, which the card names as
    /// the AST card does: a namespace path as `(anonymous)`, the scope of a
    /// class or function as clang prints it.
    auto anonymous = [](const index::SymbolInfo& symbol) {
        return index::has_flag(symbol.flags, index::SymbolFlags::AnonymousNamespace);
    };
    if(anonymous(info)) {
        hover.scope_kind = "namespace";
    } else if(!scopes.empty()) {
        hover.scope_kind = kind_noun(scopes.back().kind).str();
    }
    llvm::StringRef unnamed =
        hover.scope_kind == "namespace" ? "(anonymous)" : "(anonymous namespace)";
    llvm::SmallVector<std::string> names;
    for(const auto& scope: scopes) {
        if(anonymous(scope)) {
            names.push_back(unnamed.str());
        }
        names.push_back(scope.display_name());
    }
    if(anonymous(info)) {
        names.push_back(unnamed.str());
    }
    hover.scope = llvm::join(names, "::");
    hover.documentation = std::move(documentation);
    /// The index keeps that a symbol is deprecated, not the message.
    if(index::has_flag(info.flags, index::SymbolFlags::Deprecated)) {
        hover.deprecation.emplace();
    }
    if(definition_text.empty()) {
        return hover;
    }

    // The stored extent is the whole written definition; the AST card
    // prints a declaration — a function without its body, a namespace with
    // an empty one, a macro with its `#define`. A type's members are left
    // out: the AST card's member summary needs the AST.
    hover.definition = definition_text.str();
    switch(info.kind) {
        case SymbolKind::Function:
        case SymbolKind::Method:
        case SymbolKind::Operator: {
            if(auto head = before_body(hover.definition)) {
                hover.definition = head->str();
            }
            break;
        }
        case SymbolKind::Namespace: {
            if(auto head = before_body(hover.definition)) {
                hover.definition = head->str() + " {}";
            }
            break;
        }
        case SymbolKind::Class:
        case SymbolKind::Protocol:
        case SymbolKind::Struct:
        case SymbolKind::Union:
        case SymbolKind::Enum: {
            if(auto head = objc_head(hover.definition)) {
                hover.definition = *head + " ... @end";
            } else if(auto head = before_body(hover.definition)) {
                hover.definition = head->str() + " { ... }";
            }
            break;
        }
        case SymbolKind::Macro: {
            hover.definition.insert(0, "#define ");
            break;
        }
        default: break;
    }
    if(index::has_flag(info.flags, index::SymbolFlags::Exported)) {
        hover.definition.insert(0, "export ");
    }
    return hover;
}

auto hover_info(const index::IndexQuery& query,
                const index::IndexQuery::Cursor& cursor,
                const DocumentationOptions& documentation) -> std::optional<HoverInfo> {
    auto symbol = cursor.symbols.front();
    auto info = query.symbol_info(symbol, cursor.site.file);
    if(!info) {
        return std::nullopt;
    }
    if(info->kind == SymbolKind::Module) {
        return module_hover(query, cursor, *info, documentation);
    }
    // LIMITATION: the index holds a symbol's declared text, its scopes and
    // its comment, no type facts: the card lacks the `aka` line and the
    // value the AST's shows, while a document has no AST — evicted, its
    // worker paused or crashed, read-only, or just opened.
    std::string definition;
    auto text = index::declared_text(query, symbol, cursor.site.file, RelationKind::Definition);
    // The card of an Objective-C class or category shows its @interface,
    // as the AST's does, not the @implementation defining it.
    if(text && objc_container(text->text).starts_with("@implementation")) {
        if(auto interface =
               index::declared_text(query,
                                    symbol,
                                    cursor.site.file,
                                    RelationKind::Declaration,
                                    [](llvm::StringRef declared) {
                                        return objc_container(declared).starts_with("@interface");
                                    })) {
            text = std::move(interface);
        }
    }
    if(!text) {
        text = index::declared_text(query, symbol, cursor.site.file, RelationKind::Declaration);
    }
    if(text) {
        definition = std::move(text->text);
    }
    auto hover = hover_info(*info,
                            query.container_chain(symbol, cursor.site.file),
                            definition,
                            feature::documentation(query, symbol, cursor.site.file, documentation));
    hover.symbol_range = cursor.site.range;
    return hover;
}

auto module_hover(const index::IndexQuery& query,
                  const index::IndexQuery::Cursor& cursor,
                  const index::SymbolInfo& module,
                  const DocumentationOptions& documentation) -> HoverInfo {
    HoverInfo hover;
    hover.name = module.name;
    hover.kind = SymbolKind::Module;
    auto units =
        query.gather(module.hash,
                     cursor.site.file,
                     [&](const index::IndexQuery& from, index::SymbolHash named) {
                         return from.sites(named, cursor.site.file, RelationKind::Definition);
                     });
    // A unit's own declaration names the unit itself.
    auto providers = ranges::to<std::vector>(units | views::transform(&index::Site::file));
    if(!providers.empty()) {
        auto provider = ranges::contains(providers, cursor.site.file)
                            ? cursor.site.file
                            : nearest_provider(query.file_table(), providers, cursor.site.file);
        auto& unit = *ranges::find(units, provider, &index::Site::file);
        hover.documentation = module_documentation(query, unit, documentation);
        hover.file = query.file_table().display_identity(unit.path);
    }
    hover.symbol_range = cursor.site.range;
    return hover;
}

bool on_directive_line(llvm::StringRef text,
                       std::uint32_t offset,
                       const clang::LangOptions& dialect) {
    return Lexer(text.substr(logical_line_begin(text, offset)), dialect)
        .advance()
        .is_directive_hash();
}

auto directive_hover(llvm::StringRef text,
                     const PositionMap& map,
                     FileTable& files,
                     const feature::DocumentLink& link,
                     protocol::MarkupKind kind,
                     PositionEncoding encoding) -> std::optional<protocol::Hover> {
    if(link.range.end > text.size()) {
        return std::nullopt;
    }
    auto info = header_card(text, link.range, files.display_identity(link.target));

    auto hover = to_protocol(info, kind, map, encoding);
    if(!hover.range) {
        return std::nullopt;
    }
    return hover;
}

}  // namespace clice::feature
