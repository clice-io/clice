module clice;

import :compile.inactive_regions;
import :feature.index_entries;
import :feature.semantic_tokens;
import :feature.semantic_tokens.stream;
import :index.query;
import :index.symbol;
import :index.tu_index;
import :semantic.token_class;
import :syntax.source_text;
import :syntax.token;

namespace clice::feature {

auto semantic_tokens(SourceText& source,
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

auto semantic_tokens(SourceText& source,
                     const index::IndexQuery& query,
                     const index::IndexSource& index,
                     const SemanticTokensOptions& options,
                     std::optional<AstAnswer<std::vector<SemanticToken>>> ast)
    -> std::vector<SemanticToken> {
    auto tokens = semantic_tokens(
        source,
        extract_index_entries(*index.shard),
        [&](index::SymbolHash hash) { return query.symbol_info(hash, index.file); },
        options);
    if(!ast) {
        return tokens;
    }
    auto inactive = mask(SemanticTokenModifier::Inactive);
    auto end_of = [](const SemanticToken& token) {
        return token.range.end;
    };
    // The AST merges the adjacent tokens of inactive code it classifies
    // alike (`f()` one plain token) and the index a name of several tokens
    // (`~Widget`): each splits the other's.
    std::vector<SemanticToken> merged;
    merged.reserve(ast->answer.size());
    auto next = tokens.cbegin();
    for(const auto& token: ast->answer) {
        if(!inactive_at(ast->skipped, token.range.begin)) {
            merged.push_back(token);
            continue;
        }
        next = ranges::upper_bound(next, tokens.cend(), token.range.begin, {}, end_of);
        auto at = token.range.begin;
        for(; next != tokens.cend() && next->range.begin < token.range.end; ++next) {
            auto begin = std::max(next->range.begin, at);
            auto end = std::min(next->range.end, token.range.end);
            if(begin > at) {
                merged.push_back({
                    {at, begin},
                    token.kind,
                    token.modifiers
                });
            }
            merged.push_back({
                {begin, end},
                next->kind,
                next->modifiers | inactive
            });
            at = end;
            if(next->range.end > token.range.end) {
                break;
            }
        }
        if(at < token.range.end) {
            merged.push_back({
                {at, token.range.end},
                token.kind,
                token.modifiers
            });
        }
    }
    return merged;
}

void merge_preamble_macros(const index::TUIndex& preamble, std::vector<SemanticToken>& tokens) {
    llvm::DenseMap<std::uint32_t, std::uint32_t> modifiers;
    preamble.shard_of(preamble.focus_file())
        .for_each_occurrence([&](const index::Occurrence& occurrence) {
            auto symbol = preamble.find_symbol(occurrence.target);
            if(symbol && symbol->token_class.kind == SemanticTokenKind::Macro) {
                modifiers.try_emplace(occurrence.range.begin, symbol->token_class.modifiers);
            }
            return true;
        });
    for(auto& token: tokens) {
        if(token.kind != SemanticTokenKind::Macro) {
            continue;
        }
        if(auto it = modifiers.find(token.range.begin); it != modifiers.end()) {
            token.modifiers |= it->second;
        }
    }
}

}  // namespace clice::feature
