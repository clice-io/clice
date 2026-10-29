module clice;

import :feature.documentation;
import :feature.hover.card;
import :feature.hover;
import :index.peer_queries;
import :index.query;
import :index.symbol;
import :index.text;
import :semantic.symbol;
import :support.text;
import :syntax.documentation;
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
        case SymbolKind::Struct:
        case SymbolKind::Union:
        case SymbolKind::Enum: {
            if(auto head = before_body(hover.definition)) {
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

auto hover_info(const index::PeerQueries& queries,
                const index::IndexQuery::Cursor& cursor,
                const DocumentationOptions& documentation) -> std::optional<HoverInfo> {
    auto& query = queries.own;
    auto symbol = cursor.symbols.front();
    auto info = query.symbol_info(symbol, cursor.site.file);
    if(!info) {
        return std::nullopt;
    }
    if(info->kind == SymbolKind::Module) {
        return module_hover(queries, cursor, *info);
    }
    std::string definition;
    auto text = index::declared_text(query, symbol, cursor.site.file, RelationKind::Definition);
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

auto module_hover(const index::PeerQueries& queries,
                  const index::IndexQuery::Cursor& cursor,
                  const index::SymbolInfo& module) -> HoverInfo {
    HoverInfo hover;
    hover.name = module.name;
    hover.kind = SymbolKind::Module;
    auto units =
        queries.gather(module.hash,
                       cursor.site.file,
                       [&](const index::IndexQuery& from, index::SymbolHash named) {
                           return from.sites(named, cursor.site.file, RelationKind::Definition);
                       });
    if(!units.empty()) {
        hover.file = queries.own.file_table().display_identity(units.front().path);
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
                     const index::DocumentLink& link,
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
