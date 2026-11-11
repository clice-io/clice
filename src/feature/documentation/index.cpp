module clice;

import :feature.documentation.reading;
import :feature.documentation;
import :index.query;
import :index.query.locate;
import :index.query.site;
import :index.symbol;
import :semantic.comment;
import :syntax.lexer;
import :syntax.source_text;

namespace clice::feature {

namespace {

/// The text (comment_text) of the comment documenting the symbol, read
/// as the compiled unit's is: of its declarations, then its definitions,
/// in first-hit order, the one CommentChoice chooses. With `inherit`, a
/// method overriding another without one takes that method's.
std::string
    comment_of(const index::IndexQuery& query, index::SymbolHash hash, Fid anchor, bool inherit) {
    auto info = query.symbol_info(hash, anchor);
    SymbolKind kind = info ? info->kind : SymbolKind(SymbolKind::Invalid);
    // As the compiled unit's: a namespace has none.
    if(kind == SymbolKind::Namespace) {
        return {};
    }
    // TODO: nothing is kept across lookups: every index-served hover lexes the
    // whole source again, a few ms for ordinary headers, more for large
    // generated ones.
    // Each source's text is read and its comments scanned once, however
    // many declarations it holds.
    llvm::DenseMap<std::pair<const index::Shard*, Fid>, std::unique_ptr<SourceText>> texts;
    auto text_of = [&](const index::IndexSource& source) -> SourceText* {
        auto& text = texts[{source.shard, source.file}];
        if(!text) {
            std::string kept;
            auto read = query.source_text(source, kept);
            if(!read) {
                return nullptr;
            }
            // Owned: the lexer reads past the end of what it lexes.
            text = std::make_unique<SourceText>(read->str());
            text->set_dialect(raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx26));
        }
        return text.get();
    };
    CommentChoice choice;
    for(auto relation_kind: {RelationKind::Declaration, RelationKind::Definition}) {
        query.for_each_relation(
            hash,
            anchor,
            relation_kind,
            index::IndexQuery::Order::LiveFirst,
            {},
            [&](const index::IndexSource& source, const index::Relation& relation) {
                auto extent = relation.extent();
                auto* text = extent.begin < extent.end ? text_of(source) : nullptr;
                if(!text || extent.end > text->text().size()) {
                    return true;
                }
                auto content = text->text();
                auto decl = kind == SymbolKind::Macro
                                ? documented_macro(content, extent)
                                : DocumentedDecl{.range = extent,
                                                 .trailing = kind == SymbolKind::Field ||
                                                             kind == SymbolKind::EnumMember ||
                                                             kind == SymbolKind::Variable};
                return !choice.offer(content, attached_comments(content, text->comments(), decl));
            });
        if(choice.decided()) {
            break;
        }
    }

    if(inherit && choice.empty() && (kind == SymbolKind::Method || kind == SymbolKind::Operator)) {
        for(auto& overridden: query.targets(hash, anchor, RelationKind::Interface)) {
            if(auto text = comment_of(query, overridden.symbol, overridden.anchor, inherit);
               !text.empty()) {
                return text;
            }
        }
    }
    return std::move(choice).take();
}

/// The symbols a `@copydoc` in the comment of the symbol may name
/// (parse_copied_name), as the compiled unit's are found: from the
/// symbol's scope outward, or in the global scope; of several, the
/// functions declaring the parameters it lists, when it lists them.
auto copied_doc_targets(const index::IndexQuery& query,
                        index::SymbolHash hash,
                        Fid anchor,
                        llvm::StringRef name) -> std::vector<index::IndexQuery::Located> {
    auto copied = parse_copied_name(name);
    if(!copied) {
        return {};
    }
    auto declares = [&](const index::IndexQuery::Located& candidate) {
        auto source = query.serving(candidate.site.file);
        std::string kept;
        auto text = source ? query.source_text(*source, kept) : std::nullopt;
        return text && declares_parameters(*text, candidate.site.range.end, *copied->parameters);
    };

    auto chain = query.container_chain(hash, anchor);
    for(auto depth = copied->absolute ? 1 : chain.size() + 1; depth > 0; depth -= 1) {
        index::SymbolQuery lookup{.mode = index::SymbolQuery::Mode::Exact,
                                  .absolute = true,
                                  .pattern = copied->parts.back().str()};
        for(auto& container: llvm::ArrayRef(chain).take_front(depth - 1)) {
            lookup.scope.push_back({.name = container.name});
        }
        for(auto part: llvm::ArrayRef(copied->parts).drop_back()) {
            lookup.scope.push_back({.name = part.str()});
        }
        auto found = index::locate(query, lookup);
        if(found.empty()) {
            continue;
        }
        if(copied->parameters && found.size() > 1) {
            std::erase_if(found, [&](const index::IndexQuery::Located& candidate) {
                return !declares(candidate);
            });
        }
        return found;
    }
    return {};
}

Documentation symbol_documentation(const index::IndexQuery& query,
                                   index::SymbolHash hash,
                                   Fid anchor,
                                   const DocumentationOptions& options,
                                   unsigned depth) {
    return read_copying(
        comment_of(query, hash, anchor, options.inherit),
        options,
        depth,
        [&](llvm::StringRef name) { return copied_doc_targets(query, hash, anchor, name); },
        [&](const index::IndexQuery::Located& target, unsigned next) {
            return symbol_documentation(query, target.symbol.hash, target.site.file, options, next);
        });
}

}  // namespace

auto documentation(const index::IndexQuery& query,
                   index::SymbolHash hash,
                   Fid anchor,
                   const DocumentationOptions& options) -> Documentation {
    // A parameter shows what its function's comment says of it, as hover's
    // card does, and a template parameter, indexed as a type or a variable,
    // what its template's says.
    auto info = query.symbol_info(hash, anchor);
    if(info && info->kind == SymbolKind::Parameter && info->parent != 0) {
        auto owner = symbol_documentation(query, info->parent, anchor, options, 0);
        const auto* param = owner.param(info->name);
        return param ? Documentation{.brief = param->text} : Documentation();
    }
    if(info && info->parent != 0 &&
       (info->kind == SymbolKind::Type || info->kind == SymbolKind::Variable)) {
        auto owner = symbol_documentation(query, info->parent, anchor, options, 0);
        if(const auto* param = owner.template_param(info->name)) {
            return Documentation{.brief = param->text};
        }
    }
    return symbol_documentation(query, hash, anchor, options, 0);
}

auto module_documentation(const index::IndexQuery& query,
                          const index::Site& declaration,
                          const DocumentationOptions& options) -> Documentation {
    auto source = query.serving(declaration.file);
    std::string kept;
    auto read_text = source ? query.source_text(*source, kept) : std::nullopt;
    if(!read_text || declaration.range.end > read_text->size()) {
        return {};
    }
    // Only what precedes the declaration can document it. Owned: the lexer
    // reads past the end of what it lexes.
    SourceText text(read_text->take_front(declaration.range.end).str());
    text.set_dialect(raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx26));
    auto content = text.text();
    auto comments = attached_comments(content, text.comments(), {.range = declaration.range});
    return read(comment_text(content, comments), options, nothing_copied);
}

}  // namespace clice::feature
