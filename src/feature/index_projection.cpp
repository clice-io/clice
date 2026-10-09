module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :feature.lexical_classify;
import :index.shard;
import :syntax.lexer;

namespace clice::feature {

IndexRows extract_index_rows(const index::Shard& shard) {
    IndexRows rows;
    shard.for_each_occurrence([&](const index::Occurrence& occurrence) {
        rows.occurrences.push_back(occurrence);
        return true;
    });
    auto written = rows.occurrences;
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
        if(kind.isDeclOrDef()) {
            auto copy = relation;
            rows.decls.push_back({.range = relation.range,
                                  .extent = copy.definition_range(),
                                  .symbol = hash,
                                  .definition = kind.is_one_of(RelationKind::Definition)});
        } else if(kind.isReference() && nested(relation.range)) {
            names.push_back({.range = relation.range, .target = hash});
        }
        return true;
    });
    llvm::append_range(rows.occurrences, names);
    return rows;
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

LangProfile& profile_for(const clang::LangOptions& lang_opts) {
    return profile_for(lang_opts.CPlusPlus ? clang::Language::CXX : clang::Language::C,
                       lang_opts.LangStd);
}

bool outline_kind(SymbolKind kind) {
    switch(kind) {
        case SymbolKind::Macro:
        case SymbolKind::Module:
        case SymbolKind::Namespace:
        case SymbolKind::Class:
        case SymbolKind::Struct:
        case SymbolKind::Union:
        case SymbolKind::Enum:
        case SymbolKind::Type:
        case SymbolKind::Concept:
        case SymbolKind::Field:
        case SymbolKind::EnumMember:
        case SymbolKind::Function:
        case SymbolKind::Method:
        case SymbolKind::Operator:
        case SymbolKind::Variable: return true;
        default: return false;
    }
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

auto index_lang_options(llvm::StringRef path, bool c_rows, llvm::StringRef standard)
    -> const clang::LangOptions& {
    bool c = c_rows || path.ends_with(".c");
    auto lang = c ? clang::Language::C : clang::Language::CXX;
    // A command without -std parses under the driver's default dialect;
    // the same default keeps the keyword table aligned with the rows.
    auto kind = clang::getDefaultLanguageStandard(lang, llvm::Triple());
    if(!standard.empty()) {
        auto parsed = clang::LangStandard::getLangKind(standard);
        // A standard of the other language contradicts the resolved rows'
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
                           llvm::ArrayRef<index::Occurrence> occurrences,
                           llvm::ArrayRef<IndexDeclRow> decls,
                           IndexSymbolResolver resolve) -> std::vector<SemanticToken> {
    auto& profile = profile_for(lang_opts);

    // Rows arrive in shard order (occurrences by range, relations grouped
    // by symbol); token matching needs both sorted by anchor begin.
    std::vector<index::Occurrence> occs(occurrences.begin(), occurrences.end());
    std::ranges::sort(occs, {}, [](const index::Occurrence& o) { return o.range.begin; });
    std::vector<IndexDeclRow> decl_rows(decls.begin(), decls.end());
    std::ranges::sort(decl_rows, {}, [](const IndexDeclRow& r) { return r.range.begin; });

    // The semantic classification of the token anchored at `begin`: decl
    // rows carry their Declaration/Definition modifier; occurrences of
    // symbols without a decl row here contribute the bare kind. Multiple
    // rows on one anchor (merged shard variants) combine — kind
    // disagreement is a Conflict, like instantiation disagreement on the
    // AST path.
    auto semantic_at = [&](std::uint32_t begin) {
        Classified semantic;
        llvm::SmallVector<index::SymbolHash, 4> covered;

        auto decl_it = std::ranges::lower_bound(decl_rows, begin, {}, [](const IndexDeclRow& r) {
            return r.range.begin;
        });
        for(; decl_it != decl_rows.end() && decl_it->range.begin == begin; ++decl_it) {
            auto info = resolve(decl_it->symbol);
            if(!info) {
                continue;
            }
            auto modifier =
                decl_it->definition ? SymbolModifiers::Definition : SymbolModifiers::Declaration;
            combine(semantic, {info->kind, SymbolModifiers::to_mask(modifier)});
            covered.push_back(decl_it->symbol);
        }

        auto occ_it = std::ranges::lower_bound(occs, begin, {}, [](const index::Occurrence& o) {
            return o.range.begin;
        });
        for(; occ_it != occs.end() && occ_it->range.begin == begin; ++occ_it) {
            if(llvm::is_contained(covered, occ_it->target)) {
                continue;
            }
            if(auto info = resolve(occ_it->target)) {
                combine(semantic, {info->kind, 0});
            }
        }
        return semantic;
    };

    // A module occurrence spans the whole written name (`demo.core`,
    // `foo:part` — the index stores one row over all components), while
    // the raw lex sees one token per component; collect the spans so the
    // components past the first classify like the first, as on the AST
    // path.
    std::vector<LocalSourceRange> module_spans;
    for(const auto& occ: occs) {
        if(auto info = resolve(occ.target); info && info->kind == SymbolKind::Module) {
            module_spans.push_back(occ.range);
        }
    }

    std::vector<SemanticToken> tokens;

    enum class Directive : std::uint8_t {
        None,
        AfterHash,
        AfterDefine,
        Body,
    };
    Directive directive = Directive::None;
    bool after_line_export = false;

    Lexer lexer(content, {.keep_comments = true, .lang_opts = &profile.opts});
    while(true) {
        auto token = lexer.advance();
        if(token.is_eof()) {
            break;
        }
        if(token.is_eod()) {
            directive = Directive::None;
            continue;
        }
        if(token.kind == clang::tok::comment) {
            append_token(tokens, token.range, SymbolKind::Comment, 0);
            continue;
        }

        auto spelling = token.text(content);

        // The raw lexer leaves keywords unresolved; the profile's
        // identifier table maps them to their token kinds, which also
        // covers alternative operator spellings (and, or, not).
        auto kind = token.kind;
        if(token.is_identifier()) {
            kind = profile.keywords.get(spelling).getTokenID();
        }
        auto lexical_class = classify_lexical_kind(kind, spelling);
        Classified lexical{lexical_class.kind, 0};

        // The contextual `module` and `import` open a module declaration
        // or an import at the start of a line, behind `export` at most,
        // and before what clang requires to recognize them.
        bool line_head = token.is_at_start_of_line || after_line_export;
        after_line_export = token.is_at_start_of_line && spelling == "export";
        if(profile.opts.CPlusPlusModules && line_head &&
           (spelling == "module" || spelling == "import")) {
            auto next = lexer.next().kind;
            if((spelling == "module" &&
                llvm::is_contained(
                    {clang::tok::raw_identifier, clang::tok::colon, clang::tok::semi},
                    next)) ||
               (spelling == "import" && llvm::is_contained({clang::tok::raw_identifier,
                                                            clang::tok::colon,
                                                            clang::tok::less,
                                                            clang::tok::header_name,
                                                            clang::tok::string_literal},
                                                           next))) {
                lexical = {SymbolKind::Keyword, 0};
            }
        }

        // Directive overlay, driven by the lexer's preprocessor awareness:
        // the hash and the directive name paint as Directive, a #define's
        // name as Macro, header-name arguments as Header.
        switch(directive) {
            case Directive::None: {
                if(token.is_directive_hash()) {
                    directive = Directive::AfterHash;
                    lexical = {SymbolKind::Directive, 0};
                }
                break;
            }
            case Directive::AfterHash: {
                if(lexical_class.identifier_like) {
                    lexical = {SymbolKind::Directive, 0};
                }
                directive = spelling == "define" ? Directive::AfterDefine : Directive::Body;
                break;
            }
            case Directive::AfterDefine: {
                if(lexical_class.identifier_like) {
                    lexical = {SymbolKind::Macro, 0};
                }
                directive = Directive::Body;
                break;
            }
            case Directive::Body: break;
        }
        if(token.is_header_name()) {
            lexical = {SymbolKind::Header, 0};
        }

        // Only tokens that can spell a name take semantic classification —
        // mirroring the AST path, which anchors declaration names at
        // identifiers and a destructor's `~` only. Keywords stay lexical:
        // a row anchored on `operator` names nothing written there.
        Classified semantic;
        if(kind == clang::tok::identifier || kind == clang::tok::tilde) {
            semantic = semantic_at(token.range.begin);
        }
        if(semantic.kind == SymbolKind::Invalid && lexical_class.identifier_like &&
           !module_spans.empty()) {
            auto covering = std::ranges::upper_bound(module_spans,
                                                     token.range.begin,
                                                     {},
                                                     &LocalSourceRange::begin);
            if(covering != module_spans.begin() && token.range.end <= (covering - 1)->end) {
                semantic = semantic_at((covering - 1)->begin);
            }
        }

        Classified result = settle(semantic, lexical);

        if(result.kind != SymbolKind::Invalid) {
            append_token(tokens, token.range, result.kind, result.modifiers);
        }
    }

    return tokens;
}

auto index_document_symbols(llvm::ArrayRef<IndexDeclRow> decls, IndexSymbolResolver resolve)
    -> std::vector<DocumentSymbol> {
    struct Entry {
        DocumentSymbol symbol;
        LocalSourceRange extent;
        index::SymbolHash hash = 0;
    };

    std::vector<Entry> entries;
    for(const auto& row: decls) {
        if(!row.extent.valid() || row.extent.begin >= row.extent.end) {
            continue;
        }
        auto info = resolve(row.symbol);
        if(!info || info->name.empty() || !outline_kind(info->kind)) {
            continue;
        }
        auto selection = row.range;
        if(selection.begin < row.extent.begin || selection.end > row.extent.end) {
            selection = row.extent;
        }
        entries.push_back({
            {.name = info->display_name(),
             .kind = info->kind,
             .range = row.extent,
             .selection_range = selection},
            row.extent,
            row.symbol,
        });
    }

    // Merged shard rows can duplicate an anchor (a symbol's Declaration
    // and Definition rows of an inline definition share it, and so do
    // context variants); one outline entry per (site, symbol identity).
    // Descending end puts a strict container before everything inside it.
    std::ranges::sort(entries, [](const Entry& lhs, const Entry& rhs) {
        if(lhs.extent.begin != rhs.extent.begin) {
            return lhs.extent.begin < rhs.extent.begin;
        }
        if(lhs.extent.end != rhs.extent.end) {
            return lhs.extent.end > rhs.extent.end;
        }
        if(lhs.symbol.selection_range.begin != rhs.symbol.selection_range.begin) {
            return lhs.symbol.selection_range.begin < rhs.symbol.selection_range.begin;
        }
        if(lhs.symbol.name != rhs.symbol.name) {
            return lhs.symbol.name < rhs.symbol.name;
        }
        return lhs.hash < rhs.hash;
    });
    auto duplicates = std::ranges::unique(entries, [](const Entry& lhs, const Entry& rhs) {
        return lhs.hash == rhs.hash && lhs.extent == rhs.extent &&
               lhs.symbol.selection_range == rhs.symbol.selection_range;
    });
    entries.erase(duplicates.begin(), duplicates.end());

    // Entries are sorted by (begin asc, end desc): a strict container
    // precedes everything it contains, so a stack builds the tree; equal
    // or merely overlapping extents fail the containment test and become
    // siblings in source order.
    std::vector<DocumentSymbol> roots;
    auto contains = [](const LocalSourceRange& outer, const LocalSourceRange& inner) {
        return outer.begin <= inner.begin && inner.end <= outer.end && outer != inner;
    };
    std::vector<std::pair<Entry*, std::vector<DocumentSymbol>>> pending;

    auto close_top = [&] {
        auto [entry, children] = std::move(pending.back());
        pending.pop_back();
        entry->symbol.children = std::move(children);
        auto& into = pending.empty() ? roots : pending.back().second;
        into.push_back(std::move(entry->symbol));
    };

    for(auto& entry: entries) {
        while(!pending.empty() && !contains(pending.back().first->extent, entry.extent)) {
            close_top();
        }
        pending.emplace_back(&entry, std::vector<DocumentSymbol>{});
    }
    while(!pending.empty()) {
        close_top();
    }

    // Sibling order matches the AST outline's final sort: position
    // ascending, shorter range first.
    auto order = [](auto& self, std::vector<DocumentSymbol>& symbols) -> void {
        std::ranges::sort(symbols, [](const DocumentSymbol& lhs, const DocumentSymbol& rhs) {
            if(lhs.range.begin != rhs.range.begin) {
                return lhs.range.begin < rhs.range.begin;
            }
            return lhs.range.end < rhs.range.end;
        });
        for(auto& symbol: symbols) {
            self(self, symbol.children);
        }
    };
    order(order, roots);

    return roots;
}

auto index_folding_ranges(llvm::StringRef content,
                          const clang::LangOptions& lang_opts,
                          llvm::ArrayRef<IndexDeclRow> decls,
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
    for(const auto& row: decls) {
        if(!row.definition || !row.extent.valid() || row.extent.begin >= row.extent.end) {
            continue;
        }
        if(llvm::any_of(ambiguous, [&](const LocalSourceRange& region) {
               return region.begin < row.extent.end && row.extent.begin < region.end;
           })) {
            continue;
        }

        // The block keeps the name and signature visible like the AST
        // folds do.
        auto group = last_brace_group(braces, row.extent);
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
        if(auto info = resolve(row.symbol)) {
            kind = declaration_fold_kind(info->kind);
        }

        // Only a declaration's own block folds from its head; an
        // initializer's or a lambda's folds at its brace, as on the AST.
        ranges.push_back({
            .range = *group,
            .kind = kind,
            .collapsed_text = "{...}",
            .lines = kind ? declaration_lines(content, *group, row.range.begin, block_directives)
                          : std::nullopt,
        });
    }

    // One fold per range, the declaration's own where several rows share
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
                 llvm::StringRef definition_text,
                 llvm::StringRef comment) -> HoverInfo {
    HoverInfo hover;
    hover.name = info.display_name();
    hover.kind = info.kind;
    hover.documentation = comment.str();
    if(definition_text.empty()) {
        return hover;
    }

    // The stored extent is the whole written definition; the AST card
    // prints a declaration — a function without its body, a namespace with
    // an empty one, a macro with its `#define`. A type gets an empty body
    // too: the AST card's member summary needs the AST.
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
        case SymbolKind::Namespace:
        case SymbolKind::Class:
        case SymbolKind::Struct:
        case SymbolKind::Union:
        case SymbolKind::Enum: {
            if(auto head = before_body(hover.definition)) {
                hover.definition = head->str() + " {}";
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
