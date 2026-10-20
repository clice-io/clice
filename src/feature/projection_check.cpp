module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :feature.projection_check;
import :feature.semantic_token_stream;
import :index.indexer;
import :index.shard;
import :index.symbol;
import :index.tu_index;
import :semantic.symbol;
import :support.bytes;
import :syntax.source_text;

namespace clice::feature {

namespace {

/// The unit's own TU index, standing in for the server's index of a file
/// whose AST is not current.
struct UnitIndex {
    std::vector<std::byte> bytes;
    index::TUIndex tu;

    explicit UnitIndex(CompilationUnit& unit) :
        bytes(index::build_tu_index(unit)), tu(index::TUIndex::from_bytes(as_chars(bytes))) {}

    /// The focus file's entries; its path is the last.
    auto entries() const -> IndexEntries {
        return extract_index_entries(tu.shard_of(tu.path_count() - 1));
    }

    auto resolver() const {
        return [this](index::SymbolHash hash) -> std::optional<index::SymbolInfo> {
            auto identity = tu.find_symbol(hash);
            if(!identity) {
                return std::nullopt;
            }
            return index::SymbolInfo::from(hash, *identity);
        };
    }
};

}  // namespace

auto semantic_tokens_check(CompilationUnit& unit, const SemanticTokensOptions& options)
    -> TokenCheck {
    auto legend = semantic_token_legend(options);
    auto& source = unit.focus_file().text();
    auto content = source.text();
    auto ast =
        named_tokens(semantic_token_stream(source, semantic_token_facts(unit, {}, true), options),
                     legend);
    UnitIndex unit_index(unit);
    SourceText served(content, unit.lang_options());
    auto projected = named_tokens(
        index_semantic_tokens(served, unit_index.entries(), unit_index.resolver(), options),
        legend);

    auto spell = [](const NamedToken& token) {
        auto spelled = token.kind;
        for(const auto& modifier: token.modifiers) {
            spelled += " " + modifier;
        }
        return spelled;
    };

    std::map<std::uint32_t, const NamedToken*> by_begin;
    for(const auto& token: projected) {
        by_begin.emplace(token.range.begin, &token);
    }

    const auto& map = unit.focus_file().positions();
    TokenCheck check{
        .ast = static_cast<std::uint32_t>(ast.size()),
        .index = static_cast<std::uint32_t>(projected.size()),
    };
    auto report = [&](const NamedToken& token,
                      llvm::StringRef field,
                      std::string ast_value,
                      std::string index_value) {
        auto position = map.position(token.range.begin, PositionEncoding::UTF8);
        check.differences.push_back({
            .line = position ? position->line + 1 : 0,
            .column = position ? position->character + 1 : 0,
            .text = content.slice(token.range.begin, token.range.end).str(),
            .field = field.str(),
            .ast = std::move(ast_value),
            .index = std::move(index_value),
        });
    };
    for(const auto& token: ast) {
        auto it = by_begin.find(token.range.begin);
        if(it == by_begin.end()) {
            report(token, "only_ast", spell(token), "");
            continue;
        }
        const auto& other = *it->second;
        by_begin.erase(it);
        if(token.range.end != other.range.end) {
            report(token,
                   "range",
                   content.slice(token.range.begin, token.range.end).str(),
                   content.slice(other.range.begin, other.range.end).str());
        }
        if(token.kind != other.kind) {
            report(token, "kind", spell(token), spell(other));
        } else if(token.modifiers != other.modifiers) {
            report(token, "modifiers", spell(token), spell(other));
        }
    }
    for(auto [begin, token]: by_begin) {
        report(*token, "only_index", "", spell(*token));
    }
    std::ranges::stable_sort(check.differences, {}, [](const TokenDifference& difference) {
        return std::pair(difference.line, difference.column);
    });
    return check;
}

auto document_symbols_check(CompilationUnit& unit, const DocumentSymbolsOptions& options)
    -> OutlineCheck {
    auto ast = document_symbols(unit, options);
    UnitIndex unit_index(unit);
    SourceText served(unit.focus_file().content(), unit.lang_options());
    auto projected =
        index_document_symbols(served, unit_index.entries(), unit_index.resolver(), options);

    struct Flat {
        const DocumentSymbol* symbol;
        std::int32_t parent;
    };

    auto flatten = [](this auto& self,
                      llvm::ArrayRef<DocumentSymbol> symbols,
                      std::int32_t parent,
                      std::vector<Flat>& out) -> void {
        for(const auto& symbol: symbols) {
            auto at = static_cast<std::int32_t>(out.size());
            out.push_back({&symbol, parent});
            self(symbol.children, at, out);
        }
    };
    std::vector<Flat> left;
    std::vector<Flat> right;
    flatten(ast, -1, left);
    flatten(projected, -1, right);

    std::multimap<std::pair<std::uint32_t, std::uint32_t>, std::int32_t> by_name_range;
    for(auto [at, flat]: llvm::enumerate(right)) {
        auto selection = flat.symbol->selection_range;
        by_name_range.emplace(std::pair(selection.begin, selection.end),
                              static_cast<std::int32_t>(at));
    }
    std::vector<std::int32_t> partner(left.size(), -1);
    std::vector<bool> taken(right.size(), false);
    for(auto [at, flat]: llvm::enumerate(left)) {
        auto selection = flat.symbol->selection_range;
        auto [first, last] = by_name_range.equal_range(std::pair(selection.begin, selection.end));
        std::int32_t chosen = -1;
        for(auto it = first; it != last; ++it) {
            if(taken[it->second]) {
                continue;
            }
            if(chosen == -1) {
                chosen = it->second;
            }
            if(right[it->second].symbol->name == flat.symbol->name) {
                chosen = it->second;
                break;
            }
        }
        if(chosen != -1) {
            partner[at] = chosen;
            taken[chosen] = true;
        }
    }

    const auto& map = unit.focus_file().positions();
    auto spell_range = [&](LocalSourceRange range) {
        auto converted = map.range(range, PositionEncoding::UTF8);
        if(!converted) {
            return std::string("?");
        }
        return std::format("{}:{}-{}:{}",
                           converted->start.line + 1,
                           converted->start.character + 1,
                           converted->end.line + 1,
                           converted->end.character + 1);
    };
    auto kind_name = [](SymbolKind kind) {
        return std::string(kota::meta::enum_name(to_protocol_symbol_kind(kind), "?"));
    };
    auto parent_name = [](const std::vector<Flat>& flats, std::int32_t parent) {
        return parent == -1 ? std::string("(top level)") : flats[parent].symbol->name;
    };

    OutlineCheck check{
        .ast = static_cast<std::uint32_t>(left.size()),
        .index = static_cast<std::uint32_t>(right.size()),
    };
    auto report = [&](const DocumentSymbol& symbol,
                      llvm::StringRef field,
                      std::string ast_value,
                      std::string index_value) {
        auto position = map.position(symbol.selection_range.begin, PositionEncoding::UTF8);
        check.differences.push_back({
            .line = position ? position->line + 1 : 0,
            .column = position ? position->character + 1 : 0,
            .field = field.str(),
            .ast = std::move(ast_value),
            .index = std::move(index_value),
        });
    };
    for(auto [at, flat]: llvm::enumerate(left)) {
        const auto& lhs = *flat.symbol;
        if(partner[at] == -1) {
            report(lhs, "only_ast", std::format("{} {}", kind_name(lhs.kind), lhs.name), "");
            continue;
        }
        const auto& other = right[partner[at]];
        const auto& rhs = *other.symbol;
        if(lhs.name != rhs.name) {
            report(lhs, "name", lhs.name, rhs.name);
        }
        if(kind_name(lhs.kind) != kind_name(rhs.kind)) {
            report(lhs, "kind", kind_name(lhs.kind), kind_name(rhs.kind));
        }
        if(lhs.range != rhs.range) {
            report(lhs, "range", spell_range(lhs.range), spell_range(rhs.range));
        }
        bool same_parent =
            flat.parent == -1 ? other.parent == -1 : partner[flat.parent] == other.parent;
        if(!same_parent) {
            report(lhs, "parent", parent_name(left, flat.parent), parent_name(right, other.parent));
        }
        if(!rhs.detail.empty() && lhs.detail != rhs.detail) {
            report(lhs, "detail", lhs.detail, rhs.detail);
        }
        if(lhs.deprecated != rhs.deprecated) {
            report(lhs,
                   "deprecated",
                   lhs.deprecated ? "true" : "false",
                   rhs.deprecated ? "true" : "false");
        }
    }
    for(auto [at, flat]: llvm::enumerate(right)) {
        if(!taken[at]) {
            const auto& rhs = *flat.symbol;
            report(rhs, "only_index", "", std::format("{} {}", kind_name(rhs.kind), rhs.name));
        }
    }
    std::ranges::stable_sort(check.differences, {}, [](const OutlineDifference& difference) {
        return std::pair(difference.line, difference.column);
    });
    return check;
}

}  // namespace clice::feature
