module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :feature.semantic_token_stream;
import :index.shard;
import :semantic.token_class;
import :syntax.documentation;
import :syntax.lexer;
import :syntax.lexical_scan;
import :syntax.source_text;

namespace clice::feature {

IndexEntries extract_index_entries(const index::Shard& shard) {
    IndexEntries entries;
    shard.for_each_occurrence([&](const index::Occurrence& occurrence) {
        entries.occurrences.push_back(occurrence);
        return true;
    });
    auto written = entries.occurrences;
    std::ranges::sort(written, {}, [](const index::Occurrence& o) { return o.range.begin; });
    auto nested = [&](LocalSourceRange range) {
        auto it =
            std::ranges::upper_bound(written, range.begin, {}, [](const index::Occurrence& o) {
                return o.range.begin;
            });
        return it != written.begin() && std::prev(it)->range.contains(range) &&
               std::prev(it)->range != range;
    };
    std::vector<index::Occurrence> names;
    shard.for_each_relation([&](index::SymbolHash hash, const index::Relation& relation) {
        RelationKind kind(relation.kind);
        if(kind.carries_extent()) {
            auto copy = relation;
            auto& into = kind.isDeclOrDef() ? entries.decls : entries.instantiations;
            into.push_back({.range = relation.range,
                            .extent = copy.extent(),
                            .symbol = hash,
                            .definition = kind.is_one_of(RelationKind::Definition)});
        } else if(kind.isReference() && nested(relation.range)) {
            names.push_back({.range = relation.range, .target = hash});
        }
        if(kind.is_one_of(RelationKind::Write)) {
            entries.writes.push_back({.range = relation.range, .target = hash});
        }
        return true;
    });
    entries.occurrences.append_range(names);
    return entries;
}

namespace {

struct Brace {
    LocalSourceRange range;
    bool open;
};

/// The last balanced top-level group of `braces` (in source order) inside
/// `extent`: a function's body rather than a member-initializer's braces,
/// a tag's brace range, a namespace's block.
std::optional<LocalSourceRange> last_brace_group(llvm::ArrayRef<Brace> braces,
                                                 LocalSourceRange extent) {
    auto begin = std::ranges::lower_bound(braces, extent.begin, {}, [](const Brace& b) {
        return b.range.begin;
    });
    std::optional<LocalSourceRange> group;
    std::int32_t depth = 0;
    std::uint32_t open_begin = 0;
    for(auto it = begin; it != braces.end() && it->range.end <= extent.end; ++it) {
        if(it->open) {
            if(depth == 0) {
                open_begin = it->range.begin;
            }
            depth += 1;
        } else if(depth > 0) {
            depth -= 1;
            if(depth == 0) {
                group = LocalSourceRange{open_begin, it->range.end};
            }
        }
    }
    return group;
}

/// The braces of the code, those of directive lines left out: a #define
/// body's brace pairs nothing in the file's own text.
std::vector<Brace> braces_of(const LexicalInfo& lexical) {
    std::vector<Brace> braces;
    auto directive = lexical.directives.begin();
    for(std::uint32_t index = 0; index < lexical.tokens.size(); index += 1) {
        if(directive != lexical.directives.end() && index == directive->first) {
            index = directive->last;
            ++directive;
            continue;
        }
        const auto& token = lexical.tokens[index];
        if(token.kind == clang::tok::l_brace || token.kind == clang::tok::r_brace) {
            braces.push_back({token.range, token.kind == clang::tok::l_brace});
        }
    }
    return braces;
}

/// The text of a definition before the body its extent ends with, or
/// nullopt when the extent ends otherwise (`= default`, an initializer).
std::optional<llvm::StringRef> before_body(const std::string& text) {
    // FIXME: a function-try-block's last brace group is its last handler:
    // the card keeps the body and the `catch (...)` heads.
    auto braces = braces_of(lexical_scan(text, dialect_of()));
    auto written = llvm::StringRef(text).rtrim();
    auto body = last_brace_group(braces, {0, static_cast<std::uint32_t>(written.size())});
    if(!body || body->end != written.size()) {
        return std::nullopt;
    }
    return written.take_front(body->begin).rtrim();
}

}  // namespace

auto index_semantic_tokens(SourceText& source,
                           const IndexEntries& entries,
                           IndexSymbolResolver resolve,
                           const SemanticTokensOptions& options) -> std::vector<SemanticToken> {
    using Kind = SemanticTokenKind;
    using Modifier = SemanticTokenModifier;

    // The token kinds names_token reads.
    auto content = source.text();
    llvm::DenseMap<std::uint32_t, clang::tok::TokenKind> lexed;
    for(const auto& token: source.lexical().tokens) {
        lexed.try_emplace(token.range.begin, source.kind(token));
    }
    auto anchors = [&](std::uint32_t offset, const TokenClass& token) {
        auto it = lexed.find(offset);
        return it != lexed.end() && names_token(it->second, token);
    };

    TokenFacts facts;
    llvm::DenseMap<std::uint32_t, TokenClass> owned;
    llvm::DenseSet<std::pair<std::uint32_t, index::SymbolHash>> declared;

    // A name of several tokens its declaration owns whole (`~Widget`,
    // `operator+`, a module name) paints every token of it the AST would,
    // over the names nested in it; any other name its first token.
    auto paint = [&](LocalSourceRange range, const TokenClass& token) {
        // What a macro's expansion declares or names is spelled in its
        // definition: the expansion's name is the macro's alone.
        if(token.kind == Kind::Macro) {
            // FIXME: when variants resolve one invocation to different macros, the last
            // read wins: `empty`, `callable` and `controlFlow` follow the shards' order
            // instead of being combined like the other names.
            owned[range.begin] = token;
            return;
        }
        if(anchors(range.begin, token)) {
            combine(facts.semantic[range.begin], token);
        }
        if(token.has(Modifier::Destructor) || token.kind == Kind::Operator ||
           token.kind == Kind::Module) {
            for(auto offset = range.begin + 1; offset < range.end; offset += 1) {
                if(anchors(offset, token)) {
                    owned[offset] = token;
                }
            }
        }
    };

    for(const auto& decl: entries.decls) {
        auto info = resolve(decl.symbol);
        if(!info || info->token_class.kind == Kind::None) {
            continue;
        }
        auto token = info->token_class;
        token.modifiers |= mask(decl.definition ? Modifier::Definition : Modifier::Declaration);
        paint(decl.range, token);
        declared.insert({decl.range.begin, decl.symbol});
    }
    for(const auto& occurrence: entries.occurrences) {
        auto begin = occurrence.range.begin;
        if(declared.contains({begin, occurrence.target})) {
            continue;
        }
        auto info = resolve(occurrence.target);
        if(!info || info->token_class.kind == Kind::None) {
            continue;
        }
        // `override` and `final` lead to the methods they override without
        // spelling them.
        auto spelling = content.slice(begin, occurrence.range.end);
        if(is_override_keyword(spelling) && info->name != spelling) {
            continue;
        }
        paint(occurrence.range, info->token_class);
    }
    for(auto& [offset, token]: owned) {
        facts.semantic[offset] = token;
    }
    for(const auto& write: entries.writes) {
        if(auto it = facts.semantic.find(write.range.begin);
           it != facts.semantic.end() && it->second.kind != Kind::Macro) {
            it->second.modifiers |= mask(Modifier::Modification);
        }
    }

    return semantic_token_stream(source, facts, options);
}

auto index_document_symbols(SourceText& source,
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
    std::ranges::sort(macro_names);
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
        auto name = std::ranges::lower_bound(macro_names, directive.range.begin);
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

auto index_folding_ranges(SourceText& source,
                          llvm::ArrayRef<IndexDecl> decls,
                          IndexSymbolResolver resolve) -> std::vector<FoldingRange> {
    // The braces outside comments, literals and directive lines, and the
    // conditional levels: a branch switch or #endif observing a brace depth
    // other than its #if's means the branches unbalance braces — any raw
    // pairing is then wrong for some variant, and folds touching the region
    // are suppressed below.
    struct CondLevel {
        std::uint32_t begin;
        std::int32_t depth;
    };

    auto content = source.text();
    const auto& lexical = source.lexical();
    auto braces = braces_of(lexical);
    std::vector<LocalSourceRange> ambiguous;
    std::vector<std::uint32_t> block_directives;
    {
        using enum LexicalInfo::Directive::Kind;
        std::vector<CondLevel> levels;
        std::int32_t depth = 0;
        auto brace = braces.begin();
        auto advance_to = [&](std::uint32_t offset) {
            for(; brace != braces.end() && brace->range.begin < offset; ++brace) {
                depth += brace->open ? 1 : -1;
            }
        };
        for(const auto& directive: lexical.directives) {
            advance_to(directive.range.begin);
            if(directive.block()) {
                block_directives.push_back(directive.range.begin);
            }
            if(directive.kind == If) {
                levels.push_back({directive.range.begin, depth});
            } else if(directive.kind == Else || directive.kind == EndIf) {
                if(!levels.empty() && depth != levels.back().depth) {
                    ambiguous.push_back(
                        {levels.back().begin, lexical.words(directive).front().range.end});
                    levels.back().depth = depth;
                }
                if(directive.kind == EndIf && !levels.empty()) {
                    levels.pop_back();
                }
            }
        }
        advance_to(static_cast<std::uint32_t>(content.size()));
        for(const auto& level: levels) {
            if(depth != level.depth) {
                ambiguous.push_back({level.begin, static_cast<std::uint32_t>(content.size())});
            }
        }
    }

    std::vector<FoldingRange> ranges;
    for(const auto& decl: decls) {
        if(!decl.definition || !decl.extent.valid() || decl.extent.begin >= decl.extent.end) {
            continue;
        }
        if(std::ranges::any_of(ambiguous, [&](const LocalSourceRange& region) {
               return region.begin < decl.extent.end && decl.extent.begin < region.end;
           })) {
            continue;
        }

        // The block keeps the name and signature visible like the AST
        // folds do.
        auto group = last_brace_group(braces, decl.extent);
        if(!group) {
            continue;
        }

        // Single-line ranges are not worth folding — and the AST collector
        // drops them too, so the projection stays a subset.
        if(!content.substr(group->begin, group->length()).contains('\n')) {
            continue;
        }

        // A symbol the resolver cannot name still folds, just without a
        // kind.
        std::optional<protocol::FoldingRangeKind> kind;
        if(auto info = resolve(decl.symbol)) {
            kind = declaration_fold_kind(info->kind);
        }

        // Only a declaration's own block folds from its head; an
        // initializer's or a lambda's folds at its brace, as on the AST.
        ranges.push_back({
            .range = *group,
            .kind = kind,
            .collapsed_text = "{...}",
            .lines = kind ? declaration_lines(content, *group, decl.range.begin, block_directives)
                          : std::nullopt,
            .closer = group->end - 1,
        });
    }

    // One fold per range, the declaration's own where several entries share
    // its braces (`typedef struct {...} T;`, the struct's and the alias's).
    std::ranges::sort(ranges, [](const FoldingRange& lhs, const FoldingRange& rhs) {
        return std::tuple(lhs.range.begin, lhs.range.end, !lhs.kind, lhs.kind) <
               std::tuple(rhs.range.begin, rhs.range.end, !rhs.kind, rhs.kind);
    });
    auto duplicates =
        std::ranges::unique(ranges, [](const FoldingRange& lhs, const FoldingRange& rhs) {
            return lhs.range == rhs.range;
        });
    ranges.erase(duplicates.begin(), duplicates.end());

    return ranges;
}

auto index_document_links(SourceText& source, llvm::ArrayRef<IndexIncludeEdge> edges)
    -> std::vector<index::DocumentLink> {
    auto positions = source.positions();

    std::vector<index::DocumentLink> links;
    for(const auto& edge: edges) {
        auto line = edge.line == 0 ? std::nullopt : positions.line_bounds(edge.line - 1);
        if(!line) {
            continue;
        }
        auto range = find_directive_argument(source.lexical(), source.text(), line->begin);
        if(!range) {
            continue;
        }
        links.push_back({.range = *range, .target = edge.target});
    }

    std::ranges::sort(links, [](const index::DocumentLink& lhs, const index::DocumentLink& rhs) {
        return std::tie(lhs.range.begin, lhs.target) < std::tie(rhs.range.begin, rhs.target);
    });
    auto duplicates =
        std::ranges::unique(links,
                            [](const index::DocumentLink& lhs, const index::DocumentLink& rhs) {
                                return lhs.range == rhs.range && lhs.target == rhs.target;
                            });
    links.erase(duplicates.begin(), duplicates.end());

    return links;
}

auto index_hover(const index::SymbolInfo& info,
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

}  // namespace clice::feature
