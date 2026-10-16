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
                            .extent = copy.definition_range(),
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
    llvm::append_range(entries.occurrences, names);
    return entries;
}

namespace {

/// Lang options plus the matching keyword table; both outlive every call
/// (projections run on the master's single thread).
struct LangProfile {
    clang::LangOptions opts;
    clang::IdentifierTable keywords;

    explicit LangProfile(clang::Language lang, clang::LangStandard::Kind std) :
        opts(raw_dialect(lang, std)), keywords(opts) {}
};

LangProfile& profile_for(clang::Language lang, clang::LangStandard::Kind std) {
    // Stable addresses: callers hold the profile's opts by reference.
    static std::map<std::pair<clang::Language, clang::LangStandard::Kind>, LangProfile> profiles;
    return profiles.try_emplace({lang, std}, lang, std).first->second;
}

/// Whether `spelling` is a contextual keyword of overriding — `override`,
/// `final`, MSVC's `sealed` — which leads to the methods it overrides
/// without spelling their names.
bool overrides(llvm::StringRef spelling) {
    return spelling == "override" || spelling == "final" || spelling == "sealed";
}

LangProfile& profile_for(const clang::LangOptions& lang_opts) {
    return profile_for(lang_opts.CPlusPlus ? clang::Language::CXX : clang::Language::C,
                       lang_opts.LangStd);
}

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

/// The text of a definition before the body its extent ends with, or
/// nullopt when the extent ends otherwise (`= default`, an initializer).
std::optional<llvm::StringRef> before_body(const std::string& text) {
    std::vector<Brace> braces;
    bool in_directive = false;
    Lexer lexer(text, {.lang_opts = &index_lang_options("", false)});
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        if(token.is_eod()) {
            in_directive = false;
        } else if(token.is_directive_hash()) {
            in_directive = true;
        } else if(!in_directive &&
                  (token.kind == clang::tok::l_brace || token.kind == clang::tok::r_brace)) {
            braces.push_back({token.range, token.kind == clang::tok::l_brace});
        }
    }
    auto written = llvm::StringRef(text).rtrim();
    auto body = last_brace_group(braces, {0, static_cast<std::uint32_t>(written.size())});
    if(!body || body->end != written.size()) {
        return std::nullopt;
    }
    return written.take_front(body->begin).rtrim();
}

}  // namespace

auto index_lang_options(llvm::StringRef path, bool c_only, llvm::StringRef standard)
    -> const clang::LangOptions& {
    bool c = c_only || path.ends_with(".c");
    auto lang = c ? clang::Language::C : clang::Language::CXX;
    // A command without -std parses under the driver's default dialect;
    // the same default keeps the keyword table aligned with the index.
    auto kind = clang::getDefaultLanguageStandard(lang, llvm::Triple());
    if(!standard.empty()) {
        auto parsed = clang::LangStandard::getLangKind(standard);
        // A standard of the other language contradicts the resolved index's
        // language; keep the language's default rather than obeying it.
        if(parsed != clang::LangStandard::lang_unspecified &&
           clang::LangStandard::getLangStandardForKind(parsed).getLanguage() == lang) {
            kind = parsed;
        }
    }
    return profile_for(lang, kind).opts;
}

auto index_semantic_tokens(llvm::StringRef content,
                           const clang::LangOptions& lang_opts,
                           const IndexEntries& entries,
                           IndexSymbolResolver resolve,
                           const SemanticTokensOptions& options) -> std::vector<SemanticToken> {
    auto& profile = profile_for(lang_opts);
    using Kind = SemanticTokenKind;
    using Modifier = SemanticTokenModifier;

    // The rule the AST applies to the token a name occurrence resolves
    // to: an identifier, a destructor's `~`, or for an operator the
    // punctuation or the user-defined literal spelling it.
    llvm::DenseMap<std::uint32_t, clang::tok::TokenKind> lexed;
    Lexer lexer(content, {.lang_opts = &profile.opts});
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        auto kind = token.kind;
        if(token.is_identifier()) {
            auto it = profile.keywords.find(token.text(content));
            kind = it == profile.keywords.end() ? clang::tok::identifier : it->second->getTokenID();
        }
        lexed.try_emplace(token.range.begin, kind);
    }
    auto anchors = [&](std::uint32_t offset, const TokenClass& token) {
        auto it = lexed.find(offset);
        if(it == lexed.end()) {
            return false;
        }
        auto kind = it->second;
        return kind == clang::tok::identifier || kind == clang::tok::tilde ||
               (token.kind == Kind::Operator &&
                (clang::tok::getPunctuatorSpelling(kind) || clang::tok::isLiteral(kind)));
    };

    TokenFacts facts;
    llvm::DenseMap<std::uint32_t, TokenClass> owned;
    llvm::DenseSet<std::pair<std::uint32_t, index::SymbolHash>> declared;

    // A name of several tokens its declaration owns whole (`~Widget`,
    // `operator+`, a module name) paints every token of it the AST would,
    // over the names nested in it; any other name its first token.
    auto paint = [&](LocalSourceRange range, const TokenClass& token) {
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
        if(overrides(spelling) && info->name != spelling) {
            continue;
        }
        paint(occurrence.range, info->token_class);
    }
    for(auto& [offset, token]: owned) {
        facts.semantic[offset] = token;
    }
    for(const auto& write: entries.writes) {
        if(auto it = facts.semantic.find(write.range.begin); it != facts.semantic.end()) {
            it->second.modifiers |= mask(Modifier::Modification);
        }
    }

    return semantic_token_stream(content, profile.opts, facts, options);
}

auto index_document_symbols(llvm::StringRef content,
                            const clang::LangOptions& lang_opts,
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
    auto lexical = lexical_scan(content, &lang_opts);
    std::vector<LocalSourceRange> macros;
    for(auto directive: lexical.macro_definitions) {
        auto name = std::ranges::lower_bound(macro_names, directive.begin);
        if(name != macro_names.end() && *name < directive.end) {
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
    return outline(std::move(declarations),
                   macros,
                   modules,
                   lexical.pragma_marks,
                   content,
                   lang_opts,
                   options);
}

auto index_folding_ranges(llvm::StringRef content,
                          const clang::LangOptions& lang_opts,
                          llvm::ArrayRef<IndexDecl> decls,
                          IndexSymbolResolver resolve) -> std::vector<FoldingRange> {
    // One raw lex collects every brace outside comments, literals and
    // directive lines (a #define body's brace pairs nothing in the file's
    // own text). Conditional levels are tracked alongside: a branch switch
    // or #endif observing a brace depth other than its #if's means the
    // branches unbalance braces — any raw pairing is then wrong for some
    // variant, and folds touching the region are suppressed below.
    struct CondLevel {
        std::uint32_t begin;
        std::int32_t depth;
    };

    std::vector<Brace> braces;
    std::vector<LocalSourceRange> ambiguous;
    std::vector<std::uint32_t> block_directives;
    {
        std::vector<CondLevel> levels;
        std::int32_t depth = 0;
        std::uint32_t hash_begin = 0;
        bool after_hash = false;
        bool in_directive = false;
        Lexer lexer(content, {.lang_opts = &profile_for(lang_opts).opts});
        while(true) {
            auto token = lexer.advance();
            if(token.is_eof()) {
                break;
            }
            if(token.is_eod()) {
                after_hash = false;
                in_directive = false;
                continue;
            }
            if(token.is_directive_hash()) {
                hash_begin = token.range.begin;
                after_hash = true;
                in_directive = true;
                continue;
            }
            if(after_hash) {
                after_hash = false;
                auto spelling = token.text(content);
                if(spelling == "if" || spelling == "ifdef" || spelling == "ifndef") {
                    block_directives.push_back(hash_begin);
                    levels.push_back({hash_begin, depth});
                } else if(spelling == "elif" || spelling == "elifdef" || spelling == "elifndef" ||
                          spelling == "else" || spelling == "endif") {
                    block_directives.push_back(hash_begin);
                    if(!levels.empty() && depth != levels.back().depth) {
                        ambiguous.push_back({levels.back().begin, token.range.end});
                        levels.back().depth = depth;
                    }
                    if(spelling == "endif" && !levels.empty()) {
                        levels.pop_back();
                    }
                } else if(spelling == "pragma") {
                    auto pragma = lexer.next().text(content);
                    if(pragma == "region" || pragma == "endregion") {
                        block_directives.push_back(hash_begin);
                    }
                }
                continue;
            }
            if(in_directive) {
                continue;
            }
            if(token.kind == clang::tok::l_brace || token.kind == clang::tok::r_brace) {
                braces.push_back({token.range, token.kind == clang::tok::l_brace});
                depth += token.kind == clang::tok::l_brace ? 1 : -1;
            }
        }
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
        if(llvm::any_of(ambiguous, [&](const LocalSourceRange& region) {
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

auto index_document_links(llvm::StringRef content,
                          const clang::LangOptions& lang_opts,
                          llvm::ArrayRef<IndexIncludeEdge> edges)
    -> std::vector<index::DocumentLink> {
    auto line_starts = kota::ipc::lsp::line_starts({content.data(), content.size()});

    std::vector<index::DocumentLink> links;
    for(const auto& edge: edges) {
        if(edge.line == 0 || edge.line > line_starts.size()) {
            continue;
        }
        auto offset = line_starts[edge.line - 1];
        auto range = find_directive_argument(content, offset, &lang_opts);
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

auto index_hover(const index::SymbolRef& info,
                 llvm::ArrayRef<index::SymbolRef> scopes,
                 llvm::StringRef definition_text,
                 Documentation documentation) -> HoverInfo {
    HoverInfo hover;
    hover.name = info.display_name();
    hover.kind = info.kind;
    /// The parent chain skips anonymous namespaces, which the card names as
    /// the AST card does: a namespace path as `(anonymous)`, the scope of a
    /// class or function as clang prints it.
    auto anonymous = [](const index::SymbolRef& symbol) {
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
