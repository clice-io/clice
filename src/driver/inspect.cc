module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.command;
import :command.toolchain;
import :compile.compilation;
import :compile.tidy;
import :config.config;
import :driver.driver;
import :feature.feature;
import :feature.semantic_token_stream;
import :index.indexer;
import :index.instantiation_shard;
import :index.navigation;
import :index.query;
import :index.shard;
import :index.tu_index;
import :project.command_resolver;
import :project.configuration;
import :project.load;
import :project.project;
import :semantic.symbol;
import :support.bytes;
import :syntax.source_text;
import :vfs.file_system;
import :vfs.path;

namespace clice::driver {

namespace {

/// What every feature takes.
struct InspectOptions {
    DecoInput(meta_var = "<PATH>", help = "The source file", required = false)
    <std::vector<std::string>> path;

    DecoKV(help = R"(JSON file of the markers in the inspected source file, at byte )"
                  R"(offsets: {"points": {"name": 12}, "ranges": {"name": )"
                  R"({"begin": 3, "end": 7}}}; results are keyed by these names)",
           required = false)
    <std::string> markers;

    DecoKV(help =
               "Compile flags for the file as a JSON string array; "
               "replaces the compile_commands.json lookup",
           required = false)
    <std::string> flags;

    DecoKV(help =
               "Configuration overlay as a JSON object of clice.toml "
               "sections, over the workspace's configuration",
           required = false)
    <std::string> config_overlay;

    ConfigurationOption configuration;
};

struct FeatureOptions {
    kota::deco::decl::HelpOption help;
    InspectOptions inspect;
    LogLevelOption log;
};

/// The options of a feature that checks its own results.
struct CheckedFeatureOptions {
    kota::deco::decl::HelpOption help;
    InspectOptions inspect;

    DecoFlag(names = {"--check"},
             help =
                 "Check the results instead of printing them: report where they differ from "
                 "the same results computed another way",
             required = false)
    check;

    LogLevelOption log;
};

struct SchemaOptions {
    kota::deco::decl::HelpOption help;
    LogLevelOption log;
};

/// JSON layout of the inspect output. Field names stay snake_case (the
/// project's native spelling) and enums serialize as their C++ value
/// names; the TS side owns any mapping to LSP vocabulary. Text that is not
/// UTF-8 is written as U+FFFD, as the server writes it to the client.
struct InspectJsonConfig {
    constexpr static auto enum_repr = kota::codec::enum_repr::String;
    constexpr static auto invalid_utf8 = kota::codec::invalid_utf8::Replace;
};

struct FileEntry {
    /// The whole-document feature payload, absent when compilation failed
    /// or the feature is marker-driven.
    std::optional<kota::codec::RawValue> result;

    /// Marker-driven payloads, keyed by the names --markers gives: position
    /// features run once per point, range features once per range. A null
    /// value records a position with no result (e.g. hover on whitespace).
    std::optional<std::map<std::string, kota::codec::RawValue>> markers;

    std::optional<std::string> error;
    std::optional<std::vector<std::string>> diagnostics;
};

/// The inspected file's markers, as --markers gives them.
struct FileMarkers {
    KOTATSU_ANNOTATE(defaulted = true)
    <std::map<std::string, std::uint32_t>> points;

    KOTATSU_ANNOTATE(defaulted = true)
    <std::map<std::string, LocalSourceRange>> ranges;
};

/// `files` holds the inspected file's entry under its file name.
struct InspectOutput {
    std::string feature;
    std::map<std::string, FileEntry> files;
};

template <typename T>
std::optional<kota::codec::RawValue> to_raw_json(const T& value) {
    auto json = kota::codec::json::to_string<InspectJsonConfig>(value);
    if(!json) {
        LOG_ERROR("serialization failed: {}", json.error().message);
        return std::nullopt;
    }
    return kota::codec::RawValue{std::move(*json)};
}

/// Marker payload for hover: the feature-layer reply before the edge —
/// markdown produced by the same rendering code the server uses, with the
/// symbol range still in byte offsets.
struct HoverResult {
    std::optional<LocalSourceRange> range;
    feature::protocol::MarkupKind kind;
    std::string contents;
};

/// Strict decode for --config-overlay and --markers: an unknown key is a typo,
/// not something to silently ignore.
struct StrictJson {
    constexpr static bool deny_unknown_fields = true;
};

std::optional<kota::codec::RawValue> run_folding_ranges(CompilationUnit& unit,
                                                        [[maybe_unused]] const Config& config) {
    return to_raw_json(feature::folding_ranges(unit));
}

/// A semantic token as the client sees it: its kind and modifiers by the
/// names the legend gives them.
struct NamedToken {
    LocalSourceRange range;
    std::string kind;
    std::vector<std::string> modifiers;
};

std::vector<NamedToken> named_tokens(llvm::ArrayRef<feature::SemanticToken> tokens,
                                     const feature::SemanticTokenLegend& legend) {
    std::vector<NamedToken> named;
    for(const auto& token: tokens) {
        auto type = legend.type_of[static_cast<std::size_t>(token.kind)];
        if(!type) {
            continue;
        }
        NamedToken entry{.range = token.range, .kind = legend.types[*type]};
        auto bits = legend.bits(token.modifiers);
        for(auto [position, name]: llvm::enumerate(legend.modifiers)) {
            if(bits & (std::uint32_t(1) << position)) {
                entry.modifiers.push_back(name);
            }
        }
        named.push_back(std::move(entry));
    }
    return named;
}

std::optional<kota::codec::RawValue> run_semantic_tokens(CompilationUnit& unit,
                                                         const Config& config) {
    return to_raw_json(named_tokens(feature::semantic_tokens(unit, config.semantic_tokens),
                                    feature::semantic_token_legend(config.semantic_tokens)));
}

/// One way the tokens the index projects differ from the AST's, at the
/// token it concerns (1-based line and column).
struct TokenDifference {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string text;
    /// only_ast, only_index, range, kind or modifiers.
    std::string field;
    std::string ast;
    std::string index;
};

struct TokenCheck {
    std::uint32_t ast = 0;
    std::uint32_t index = 0;
    std::vector<TokenDifference> differences;
};

/// The file's semantic tokens as the AST gives them beside the ones the
/// index projects from the unit's own TU index, as the server serves a file
/// before its AST is current: tokens pair by where they begin.
std::optional<kota::codec::RawValue> run_semantic_tokens_check(CompilationUnit& unit,
                                                               const Config& config) {
    auto legend = feature::semantic_token_legend(config.semantic_tokens);
    auto& source = unit.focus_file().text();
    auto content = source.text();
    auto ast =
        named_tokens(feature::semantic_token_stream(source,
                                                    feature::semantic_token_facts(unit, {}, true),
                                                    config.semantic_tokens),
                     legend);
    auto bytes = index::build_tu_index(unit);
    auto tu = index::TUIndex::from_bytes(as_chars(bytes));
    SourceText served(content, unit.lang_options());
    auto projected =
        named_tokens(feature::index_semantic_tokens(
                         served,
                         feature::extract_index_entries(tu.shard_of(tu.path_count() - 1)),
                         [&](index::SymbolHash hash) -> std::optional<index::SymbolInfo> {
                             auto identity = tu.find_symbol(hash);
                             if(!identity) {
                                 return std::nullopt;
                             }
                             return index::SymbolInfo::from(hash, *identity);
                         },
                         config.semantic_tokens),
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
    return to_raw_json(check);
}

std::optional<kota::codec::RawValue> run_document_symbols(CompilationUnit& unit,
                                                          const Config& config) {
    return to_raw_json(feature::document_symbols(unit, config.document_symbols));
}

/// One way the outline the index projects differs from the AST's, at the
/// name of the entry it concerns (1-based line and column).
struct OutlineDifference {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    /// only_ast, only_index, name, kind, range, parent, detail or
    /// deprecated.
    std::string field;
    std::string ast;
    std::string index;
};

struct OutlineCheck {
    std::uint32_t ast = 0;
    std::uint32_t index = 0;
    std::vector<OutlineDifference> differences;
};

/// The file's outline as the AST gives it beside the one the index projects
/// from the unit's own TU index, as the server serves a file before its AST
/// is current: an entry of one matches the other's entry of the same name
/// range, the same name first. The index knows no signatures, so a detail
/// differs only where the index gives one.
std::optional<kota::codec::RawValue> run_document_symbol_check(CompilationUnit& unit,
                                                               const Config& config) {
    auto ast = feature::document_symbols(unit, config.document_symbols);
    auto bytes = index::build_tu_index(unit);
    auto tu = index::TUIndex::from_bytes(as_chars(bytes));
    SourceText served(unit.focus_file().content(), unit.lang_options());
    auto projected = feature::index_document_symbols(
        served,
        feature::extract_index_entries(tu.shard_of(tu.path_count() - 1)),
        [&](index::SymbolHash hash) -> std::optional<index::SymbolInfo> {
            auto identity = tu.find_symbol(hash);
            if(!identity) {
                return std::nullopt;
            }
            return index::SymbolInfo::from(hash, *identity);
        },
        config.document_symbols);

    struct Flat {
        const feature::DocumentSymbol* symbol;
        std::int32_t parent;
    };

    auto flatten = [](this auto& self,
                      llvm::ArrayRef<feature::DocumentSymbol> symbols,
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
        return std::string(kota::meta::enum_name(feature::to_protocol_symbol_kind(kind), "?"));
    };
    auto parent_name = [](const std::vector<Flat>& flats, std::int32_t parent) {
        return parent == -1 ? std::string("(top level)") : flats[parent].symbol->name;
    };

    OutlineCheck check{
        .ast = static_cast<std::uint32_t>(left.size()),
        .index = static_cast<std::uint32_t>(right.size()),
    };
    auto report = [&](const feature::DocumentSymbol& symbol,
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
    return to_raw_json(check);
}

std::optional<kota::codec::RawValue> run_document_links(CompilationUnit& unit,
                                                        [[maybe_unused]] const Config& config) {
    return to_raw_json(feature::document_links(unit));
}

struct RawLocation {
    std::string path;
    kota::ipc::protocol::Range range;
};

struct RawInlayHintPart {
    std::string value;
    /// Where the piece links, for one naming a symbol the index places or
    /// a target in the focus file.
    std::optional<RawLocation> location;
};

struct RawInlayHint {
    std::uint32_t offset = 0;
    feature::HintCategory kind;
    std::vector<RawInlayHintPart> label;
    bool padding_left = false;
    bool padding_right = false;
};

/// Range payload for inlay hints: each label piece naming a symbol placed
/// as the server places it, from the unit's own TU index standing in for
/// the server's — a declaration, else the definition, the focus file's shard
/// first as an open buffer's is.
std::optional<kota::codec::RawValue> run_inlay_hints(CompilationUnit& unit,
                                                     LocalSourceRange range,
                                                     const Config& config) {
    auto hints = feature::inlay_hints(unit, range, config.inlay_hints);

    std::vector<std::byte> bytes;
    index::TUIndex tu;
    auto place = [&](index::SymbolHash symbol) -> std::optional<RawLocation> {
        if(bytes.empty()) {
            bytes = index::build_tu_index(unit, {.all_files = true});
            tu = index::TUIndex::from_bytes(as_chars(bytes));
        }
        auto focus = tu.path_count() - 1;
        for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
            for(std::uint32_t i = 0; i < tu.path_count(); i += 1) {
                auto path_id = i == 0 ? focus : i - 1;
                const auto& shard = tu.shard_of(path_id);
                std::optional<LocalSourceRange> found;
                shard.relations_of(symbol, kind, [&](const index::Relation& relation) {
                    found = relation.range;
                    return false;
                });
                if(!found) {
                    continue;
                }
                return RawLocation{
                    .path = tu.path(path_id).str(),
                    .range = *shard.positions().range(*found, PositionEncoding::UTF16),
                };
            }
        }
        return std::nullopt;
    };

    std::vector<RawInlayHint> out;
    for(auto& hint: hints) {
        RawInlayHint raw{
            .offset = hint.offset,
            .kind = hint.kind,
            .padding_left = hint.padding_left,
            .padding_right = hint.padding_right,
        };
        for(auto& part: hint.label) {
            raw.label.push_back({.value = std::move(part.value)});
            if(part.symbol) {
                raw.label.back().location = place(part.symbol);
            } else if(part.target) {
                auto position =
                    unit.focus_file().positions().position(*part.target, PositionEncoding::UTF16);
                raw.label.back().location = RawLocation{
                    .path = unit.file_path(unit.focus_file().file()).str(),
                    .range = {*position, *position},
                };
            }
        }
        out.push_back(std::move(raw));
    }
    return to_raw_json(out);
}

std::optional<kota::codec::RawValue> run_selection_range(CompilationUnit& unit,
                                                         std::uint32_t offset,
                                                         [[maybe_unused]] const Config& config) {
    return to_raw_json(feature::selection_ranges(unit, {offset}).front());
}

struct RawCodeAction {
    std::string title;
    std::string kind;
    std::vector<TextReplacement> edits;
    /// The definitions a host-source request would place; the request
    /// itself stays unresolved here, having no host and no index.
    std::optional<std::vector<std::string>> host_definitions;
};

/// The index requests resolved as an empty index would: definitions into
/// the focus file are all kept, no header declares any name.
std::optional<kota::codec::RawValue> run_code_action(CompilationUnit& unit,
                                                     LocalSourceRange selection,
                                                     [[maybe_unused]] const Config& config) {
    std::vector<RawCodeAction> out;
    auto path = unit.file_path(unit.focus_file().file());
    for(auto& action: feature::code_actions(unit, selection)) {
        RawCodeAction raw{
            .title = std::move(action.title),
            .kind = std::string(action.kind),
            .edits = std::move(action.edits),
        };
        if(action.index) {
            auto keep = [](std::uint64_t) {
                return false;
            };
            if(auto* request = std::get_if<feature::DefineRequest>(&*action.index)) {
                auto text = feature::assemble_definitions(request->pieces, keep);
                raw.edits = feature::format_edits(
                    path,
                    unit.focus_file().content(),
                    {
                        {request->range, request->before + *text + request->after}
                });
            } else if(auto* host = std::get_if<feature::DefineInHostRequest>(&*action.index)) {
                raw.host_definitions.emplace();
                for(const auto& piece: host->pieces) {
                    raw.host_definitions->push_back(piece.text);
                }
            } else {
                continue;
            }
        }
        out.push_back(std::move(raw));
    }
    return to_raw_json(out);
}

/// nullopt = serialization failure; an empty RawValue serializes as null
/// and records a marker with no hover. The card is markdown, as a client
/// that shows markdown gets it.
std::optional<kota::codec::RawValue> run_hover(CompilationUnit& unit,
                                               std::uint32_t offset,
                                               const Config& config) {
    auto info = feature::hover_info(unit, offset, config.hover, config.documentation);
    if(!info) {
        return kota::codec::RawValue{};
    }
    return to_raw_json(HoverResult{info->symbol_range,
                                   feature::protocol::MarkupKind::Markdown,
                                   info->present().as_markdown()});
}

/// Completion-shaped features (code completion, signature help) drive
/// their own compilation: the offset parameterizes the parse itself, so
/// each point gets a fresh completion compile instead of a query
/// against one shared unit. `params` arrives fully configured except for
/// the completion offset.
std::optional<kota::codec::RawValue> run_code_completion(CompilationParams params,
                                                         const Config& config) {
    // The replies of an editor client that takes everything an item can carry.
    return to_raw_json(
        feature::code_complete(std::move(params),
                               config.code_completion,
                               config.documentation,
                               {.snippets = true, .insert_replace = true, .markdown = true})
            .list);
}

std::optional<kota::codec::RawValue> run_signature_help(CompilationParams params,
                                                        const Config& config) {
    return to_raw_json(
        feature::signature_help(std::move(params), config.signature_help, config.documentation));
}

/// Marker payload for document-highlight: the unit's own TU index stands
/// in for the server's file index, the cursor resolved by the same rule.
std::optional<kota::codec::RawValue> run_document_highlight(CompilationUnit& unit,
                                                            std::uint32_t offset,
                                                            [[maybe_unused]] const Config& config) {
    auto bytes = index::build_tu_index(unit);
    auto tu = index::TUIndex::from_bytes(as_chars(bytes));
    const index::Shard& shard = tu.shard_of(tu.path_count() - 1);
    auto named = index::named_at(shard, offset, [&](index::SymbolHash hash) {
        auto symbol = tu.find_symbol(hash);
        return symbol && symbol->kind == SymbolKind::Macro;
    });
    llvm::SmallVector<const index::InstantiationShard*, 1> instantiations;
    if(auto section = tu.instantiation_section_of(tu.path_count() - 1)) {
        instantiations.push_back(&tu.instantiation_shard(*section));
        index::add_resolved(*instantiations.front(), offset, named);
    }
    if(!named) {
        return to_raw_json(std::vector<index::Highlight>{});
    }
    return to_raw_json(index::highlights({&shard},
                                         instantiations,
                                         named->symbols,
                                         [&](index::SymbolHash hash) -> std::optional<SymbolKind> {
                                             if(auto symbol = tu.find_symbol(hash)) {
                                                 return symbol->kind;
                                             }
                                             return std::nullopt;
                                         }));
}

/// A feature runs in exactly one shape: whole-document (`run`), once per
/// point against a shared unit (`run_at`), once per range with a
/// whole-document default (`run_over`), once per point (an empty
/// selection) and per range with no default (`run_select`), or once per
/// point with its own completion compile (`run_complete`). `check` is what
/// --check runs instead, for a feature that checks its own results.
struct FeatureSpec {
    llvm::StringRef name;
    llvm::StringRef description;
    std::optional<kota::codec::RawValue> (*run)(CompilationUnit&, const Config&) = nullptr;
    std::optional<kota::codec::RawValue> (*run_at)(CompilationUnit&,
                                                   std::uint32_t,
                                                   const Config&) = nullptr;
    std::optional<kota::codec::RawValue> (*run_over)(CompilationUnit&,
                                                     LocalSourceRange,
                                                     const Config&) = nullptr;
    std::optional<kota::codec::RawValue> (*run_select)(CompilationUnit&,
                                                       LocalSourceRange,
                                                       const Config&) = nullptr;
    std::optional<kota::codec::RawValue> (*run_complete)(CompilationParams,
                                                         const Config&) = nullptr;
    const FeatureSpec* check = nullptr;
};

constexpr FeatureSpec document_symbols_check{.name = "document-symbols",
                                             .run = run_document_symbol_check};
constexpr FeatureSpec semantic_tokens_check{.name = "semantic-tokens",
                                            .run = run_semantic_tokens_check};

constexpr std::array features = {
    FeatureSpec{.name = "code-action",
                .description = "The code actions on a selection",
                .run_select = run_code_action},
    FeatureSpec{.name = "code-completion",
                .description = "The completions at a point",
                .run_complete = run_code_completion},
    FeatureSpec{.name = "document-highlight",
                .description = "The places naming the symbol at a point",
                .run_at = run_document_highlight},
    FeatureSpec{.name = "document-links",
                .description = "The file's links",
                .run = run_document_links},
    FeatureSpec{.name = "document-symbols",
                .description = "The file's outline",
                .run = run_document_symbols,
                .check = &document_symbols_check},
    FeatureSpec{.name = "folding-range",
                .description = "The file's folding ranges",
                .run = run_folding_ranges},
    FeatureSpec{.name = "hover", .description = "The hover card at a point", .run_at = run_hover},
    FeatureSpec{.name = "inlay-hint",
                .description = "The inlay hints over a range, else the file",
                .run_over = run_inlay_hints},
    FeatureSpec{.name = "selection-range",
                .description = "The selection ranges at a point",
                .run_at = run_selection_range},
    FeatureSpec{.name = "semantic-tokens",
                .description = "The file's semantic tokens",
                .run = run_semantic_tokens,
                .check = &semantic_tokens_check},
    FeatureSpec{.name = "signature-help",
                .description = "The signature help at a point",
                .run_complete = run_signature_help},
};

/// The inspected file, with the markers --markers places in it.
struct SourceFile {
    std::string path;
    std::string content;
    FileMarkers markers;
};

/// The command of the unit writing the precompiled header a command reads
/// (Build::pch_unit).
struct PCHWriter {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
};

/// The compile command for one file.
struct FileCommand {
    std::vector<std::string> arguments;
    std::string directory;
    std::string workspace;
    std::shared_ptr<const SynthesizedContext> synthesized;
    std::optional<PCHWriter> pch_writer;

    /// clice's build of pch_writer's header, which the compile loads.
    std::string pch;
};

/// The workspace the inspected file's directory belongs to: itself or the
/// nearest ancestor holding a configuration file or a compile_commands.json
/// (the upward lookup clangd does), so a nested tree resolves its commands
/// as the server would from the project root; `start` itself when none does.
/// Only the ancestors themselves are checked — scanning their
/// subdirectories would let an unrelated sibling project's database win.
CanonicalPath workspace_of(CanonicalRef start) {
    CanonicalPath workspace = start;
    path::walk_ancestors(start, [&](CanonicalRef dir) {
        bool marked = llvm::any_of(config_file_names,
                                   [&](llvm::StringRef marker) {
                                       return vfs::exists(path::join(dir, marker));
                                   }) ||
                      vfs::exists(path::join(dir, "compile_commands.json"));
        if(marked) {
            workspace = dir;
        }
        return !marked;
    });
    return workspace;
}

/// The compile command for `file`: explicit --flags arguments, else the
/// resolution the server performs for a background compile: the file's
/// entry, a host's command for a header, a rule's default command, the
/// builtin fallback. On failure records the error on `entry` and returns
/// nullopt.
std::optional<FileCommand> file_command(FileEntry& entry,
                                        const std::string& file,
                                        llvm::ArrayRef<std::string> flags,
                                        llvm::StringRef flags_directory,
                                        Project& project,
                                        CommandResolver& commands) {
    namespace types = clang::driver::types;
    auto type = suffix_type(file);
    bool is_header = type == types::TY_CHeader || type == types::TY_CXXHeader;

    FileCommand command;
    if(!flags.empty()) {
        bool is_cxx = type != types::TY_INVALID && types::isCXX(type);
        std::vector<const char*> driver_args = {is_cxx || is_header ? "clang++" : "clang"};
        if(is_header) {
            // An ambiguous header is C++ by default, like clangd; -x forces
            // TU semantics instead of a precompiled-header job.
            driver_args.insert(driver_args.end(), {"-x", "c++"});
        }
        // The toolchain query spawns the driver from the process cwd, and the
        // driver reads @response-files itself — resolve them against the
        // file's directory so they don't depend on where inspect was launched.
        std::vector<std::string> resolved_flags(flags.begin(), flags.end());
        for(auto& flag: resolved_flags) {
            llvm::StringRef rest(flag);
            if(rest.consume_front("@") && !path::is_absolute(rest)) {
                llvm::SmallString<256> abs(flags_directory);
                path::append(abs, rest);
                flag = ("@" + abs).str();
            }
        }
        for(auto& flag: resolved_flags) {
            driver_args.push_back(flag.c_str());
        }
        driver_args.insert(driver_args.end(), {"-fsyntax-only", file.c_str()});
        auto cc1 = Toolchain::query(driver_args, file);
        if(!cc1) {
            entry.error = "toolchain_error";
            entry.diagnostics = {std::move(cc1.error())};
            return std::nullopt;
        }
        command.arguments = std::move(*cc1);
        command.directory = flags_directory.str();
        return command;
    }

    auto path_id = project.file_table.intern(Spelling::absolute(file));
    auto resolution = commands.resolve_command(path_id, command.directory, command.arguments);
    command.synthesized = resolution.synthesized;
    if(auto writer = commands.pch_unit(path_id, resolution)) {
        auto argv = project.cdb.render(*writer);
        command.pch_writer = PCHWriter{
            .file = std::string(project.file_table.resolve(writer->file)),
            .directory = project.cdb.config(writer->config).directory,
            .arguments = {argv.begin(), argv.end()},
        };
    }
    return command;
}

/// clice's build of the precompiled header the command reads, as the server
/// builds it: under its writer's command the header set holds what the
/// compiler compiled, which the unit's own flags may not (a target reusing
/// another target's PCH). Empty when it does not build, or when it reads
/// `file` itself, whose text its guard would then skip.
std::string build_pch(const PCHWriter& writer,
                      llvm::StringRef file,
                      llvm::StringRef workspace,
                      FileTable& files) {
    auto output = vfs::temp_file("clice-inspect", "pch");
    if(!output) {
        LOG_WARN("cannot create a temporary PCH: {}", output.error().message());
        return {};
    }
    auto unit = clice::compile({
        .kind = CompilationKind::PrecompiledHeader,
        .input = {.arguments = writer.arguments,
                  .directory = writer.directory,
                  .workspace = workspace.str()},
        .output_file = *output,
    });
    // The writer's own source is no dep of its unit, but the PCH holds it.
    auto self = files.intern(Spelling::absolute(file));
    bool reads = files.intern(Spelling::absolute(writer.file)) == self ||
                 llvm::any_of(unit->deps(), [&](const DepFile& dep) {
                     return files.intern(Spelling::absolute(dep.path)) == self;
                 });
    bool built = unit->has_output() && !reads;
    // Destroying the unit writes the PCH to disk.
    unit.reset();
    if(!built) {
        vfs::remove(*output);
        return {};
    }
    return *output;
}

/// Run the feature over the file and fill its entry. The compile is one
/// pass, deliberately without the preamble PCH the server uses: a shared
/// snapshot pins that the PCH split does not change feature results, so any
/// divergence between the two paths shows as a snapshot mismatch instead of
/// hiding in the preamble. The precompiled header the build declares it
/// loads (build_pch). With `diagnostics.clang_tidy` the compile runs the
/// file's clang-tidy configuration as the server's does.
void run_feature(FileEntry& entry,
                 const FeatureSpec& spec,
                 const SourceFile& file,
                 const FileCommand& command,
                 const Config& config) {
    CompilationInput input{
        .arguments = command.arguments,
        .directory = command.directory,
        .workspace = command.workspace,
    };
    input.add_focus(file.path, file.content);
    if(command.synthesized) {
        input.add_synthesized(command.synthesized->files);
    }
    if(!command.pch.empty()) {
        input.pch = {command.pch, 0};
    }

    CompilationParams params{.kind = CompilationKind::Content, .input = input};
    if(config.diagnostics.clang_tidy) {
        auto resolution = tidy::resolve_tidy_params(file.path);
        if(!resolution.error.empty()) {
            LOG_WARN("{}", resolution.error);
        }
        params.tidy = std::move(resolution.params);
    }

    auto unit = clice::compile(std::move(params));
    if(!unit->completed()) {
        entry.error = "compile_error";
        auto& messages = entry.diagnostics.emplace();
        for(const auto& diagnostic: unit->diagnostics()) {
            messages.push_back(diagnostic.message);
            for(const auto& note: diagnostic.notes) {
                messages.push_back(note.message);
            }
        }
        return;
    }

    // The AST builds even for broken sources (a language server must keep
    // working on them), so error diagnostics are reported separately: the
    // test harness rejects code that silently broke instead of pinning
    // garbage.
    if(auto errors = collect_errors(*unit); !errors.empty()) {
        entry.diagnostics = std::move(errors);
    }

    const auto& [points, ranges] = file.markers;
    if(spec.run_complete != nullptr) {
        // Completion shape: the plain compile above only serves the
        // clean-code gate — the completion entry points discard their
        // unit, so its diagnostics are the sole health signal. Each point
        // then compiles again with the offset applied.
        if(points.empty()) {
            entry.error = "no_markers";
            return;
        }
        std::map<std::string, kota::codec::RawValue> markers;
        for(const auto& [name, offset]: points) {
            auto value = spec.run_complete(
                {
                    .kind = CompilationKind::Completion,
                    .input = input,
                    .completion = {.offset = offset},
                },
                config);
            if(!value.has_value()) {
                entry.error = "serialize_error";
                return;
            }
            markers.emplace(name, std::move(*value));
        }
        entry.markers = std::move(markers);
        return;
    }

    if(spec.run != nullptr) {
        entry.result = spec.run(*unit, config);
        if(!entry.result.has_value()) {
            entry.error = "serialize_error";
        }
        return;
    }

    if(spec.run_at != nullptr) {
        // Position feature: run once per point. Without any point there is
        // nothing to answer — an error, not an empty result.
        if(points.empty()) {
            entry.error = "no_markers";
            return;
        }
        std::map<std::string, kota::codec::RawValue> markers;
        for(const auto& [name, offset]: points) {
            auto value = spec.run_at(*unit, offset, config);
            if(!value.has_value()) {
                entry.error = "serialize_error";
                return;
            }
            markers.emplace(name, std::move(*value));
        }
        entry.markers = std::move(markers);
        return;
    }

    // Range and selection features run once per range; a selection feature
    // also once per point, as an empty selection, and has no whole-document
    // default.
    std::vector<std::pair<std::string, LocalSourceRange>> places(ranges.begin(), ranges.end());
    auto run = spec.run_select != nullptr ? spec.run_select : spec.run_over;
    if(spec.run_select != nullptr) {
        for(const auto& [name, offset]: points) {
            places.emplace_back(name, LocalSourceRange(offset, offset));
        }
        if(places.empty()) {
            entry.error = "no_markers";
            return;
        }
    }
    if(places.empty()) {
        entry.result = run(*unit,
                           LocalSourceRange(0, static_cast<std::uint32_t>(file.content.size())),
                           config);
        if(!entry.result.has_value()) {
            entry.error = "serialize_error";
        }
        return;
    }
    std::map<std::string, kota::codec::RawValue> markers;
    for(const auto& [name, range]: places) {
        auto value = run(*unit, range, config);
        if(!value.has_value()) {
            entry.error = "serialize_error";
            return;
        }
        markers.emplace(name, std::move(*value));
    }
    entry.markers = std::move(markers);
}

int run_inspect(const InspectOptions& opts, const FeatureSpec& spec) {
    auto args = positional(opts.path.as_optional(), {"<PATH>"});
    if(!args) {
        return usage_error(Errors::Text, args.error());
    }
    Spelling abs_path(args->front(), Spelling::cwd());
    if(!vfs::exists(abs_path)) {
        LOG_ERROR("no such file or directory: {}", abs_path);
        return 1;
    }
    if(vfs::is_directory(abs_path)) {
        LOG_ERROR("{} is a directory, not a source file", abs_path);
        return 1;
    }

    SourceFile file{.path = abs_path.str()};
    if(opts.markers.has_value()) {
        auto buffer = vfs::read(*opts.markers);
        if(!buffer) {
            LOG_ERROR("cannot read --markers {}: {}", *opts.markers, buffer.error().message());
            return 1;
        }
        auto result =
            kota::codec::json::from_string<StrictJson>((*buffer)->getBuffer(), file.markers);
        if(!result) {
            LOG_ERROR("invalid --markers: {}", result.error().message);
            return 1;
        }
        if(file.markers.points.contains("") || file.markers.ranges.contains("")) {
            LOG_ERROR("--markers gives every marker a name");
            return 1;
        }
        // A selection feature answers points and ranges under one name each.
        for(const auto& name: file.markers.points | std::views::keys) {
            if(file.markers.ranges.contains(name)) {
                LOG_ERROR("--markers names a point and a range {}", name);
                return 1;
            }
        }
    }

    std::vector<std::string> flags;
    if(opts.flags.has_value()) {
        if(auto result = kota::codec::json::from_string(*opts.flags, flags); !result) {
            LOG_ERROR("--flags is not a JSON string array: {}", result.error().message);
            return 1;
        }
        if(flags.empty()) {
            LOG_ERROR("--flags must name at least one compile flag");
            return 1;
        }
    }

    // The file belongs to the nearest project at or above it: its
    // configuration, the databases it names (or the one discovered under
    // it) and the dependency graph give the file the command the server
    // would use — the same loading path as `clice serve`.
    auto directory = abs_path.parent();
    FileTable file_table;
    Project project{file_table};
    CommandResolver commands(project);
    if(!flags.empty() && opts.configuration.configuration) {
        LOG_ERROR("--configuration selects among the workspace's rules; --flags replaces them");
        return 1;
    }
    auto overlay = [&] {
        if(!opts.config_overlay) {
            return true;
        }
        auto result =
            kota::codec::json::from_string<StrictJson>(*opts.config_overlay, project.config);
        if(!result) {
            LOG_ERROR("invalid --config-overlay: {}", result.error().message);
        }
        return result.has_value();
    };
    if(flags.empty()) {
        auto identity = CanonicalPath(directory);
        auto root = workspace_of(identity);
        // The overlay goes in before the rules compile, as initializationOptions do.
        project.config = Config::load_from_workspace(root, nullptr, nullptr, /*finalized=*/false);
        if(!overlay()) {
            return 1;
        }
        project.config.finalize(root);
        project.config.keep_own_cache_dir();
        auto requested = opts.configuration.tag();
        if(auto checked = check_requested_configuration(project.config, requested); !checked) {
            LOG_ERROR("{}", checked.error());
            return 1;
        }
        // What the server discovers when the file is opened: the databases
        // between its directory and the root.
        load_build(project,
                   root,
                   resolve_configuration(project.config, requested),
                   compile_commands_above(identity, root));
    } else if(!overlay()) {
        return 1;
    }

    auto name = path::filename(file.path).str();
    if(is_other_language_path(file.path)) {
        LOG_ERROR("{}: not a C-family source file", name);
        return 1;
    }
    auto buffer = vfs::read(file.path);
    if(!buffer) {
        LOG_ERROR("cannot read {}: {}", abs_path, buffer.error().message());
        return 1;
    }
    file.content = (*buffer)->getBuffer().str();
    auto inside = [&](std::uint32_t offset) {
        return offset <= file.content.size();
    };
    if(!std::ranges::all_of(file.markers.points | std::views::values, inside) ||
       !std::ranges::all_of(file.markers.ranges | std::views::values,
                            [&](const LocalSourceRange& range) {
                                return range.begin <= range.end && inside(range.end);
                            })) {
        LOG_ERROR("--markers puts a marker outside the text of {}", name);
        return 1;
    }

    InspectOutput output;
    output.feature = spec.name.str();
    auto& entry = output.files[name];
    auto command = file_command(entry, file.path, flags, directory, project, commands);
    if(command) {
        command->workspace = project.config.workspace_root.str();
        if(command->pch_writer) {
            command->pch =
                build_pch(*command->pch_writer, file.path, command->workspace, file_table);
        }
        run_feature(entry, spec, file, *command, project.config);
        if(!command->pch.empty()) {
            vfs::remove(command->pch);
        }
    }

    auto json = kota::codec::json::to_string<InspectJsonConfig>(output);
    if(!json) {
        LOG_ERROR("serialization failed: {}", json.error().message);
        return 1;
    }
    driver::println("{}", *json);
    return 0;
}

CommandHandler build(const char*) {
    std::vector<Operation> operations;
    for(const auto& spec: features) {
        auto usage = std::format("clice inspect {} <PATH> [OPTIONS]", spec.name);
        operations.push_back({
            .name = spec.name,
            .description = spec.description,
            .run = spec.check
                       ? operation<CheckedFeatureOptions>(
                             usage,
                             "inspect",
                             Errors::Text,
                             [&spec](const CheckedFeatureOptions& opts) {
                                 return run_inspect(opts.inspect, opts.check ? *spec.check : spec);
                             })
                       : operation<FeatureOptions>(usage,
                                                   "inspect",
                                                   Errors::Text,
                                                   [&spec](const FeatureOptions& opts) {
                                                       return run_inspect(opts.inspect, spec);
                                                   }),
        });
    }
    operations.push_back({
        .name = "config-schema",
        .description = "The JSON schema of the clice configuration",
        .run = operation<SchemaOptions>("clice inspect config-schema [OPTIONS]",
                                        "inspect",
                                        Errors::Text,
                                        [](const SchemaOptions&) {
                                            auto schema = Config::json_schema();
                                            if(!schema) {
                                                LOG_ERROR("config schema generation failed: {}",
                                                          schema.error());
                                                return 1;
                                            }
                                            driver::println("{}", *schema);
                                            return 0;
                                        }),
    });
    return with_operations("Run a feature on a source file and print raw results as JSON",
                           std::move(operations),
                           Errors::Text);
}

[[maybe_unused]] const bool registered =
    register_command(Command::inspect,
                     {.description = "Run a feature on a source file and print raw results as JSON",
                      .build = build});

}  // namespace

}  // namespace clice::driver
