module;

#include "support/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.file_kind;
import :command.table;
import :compile.compilation;
import :compile.tidy;
import :config.config;
import :driver.driver;
import :feature.code_action.code_action;
import :feature.code_completion;
import :feature.document_links;
import :feature.document_symbols;
import :feature.folding_ranges;
import :feature.hover;
import :feature.index_check;
import :feature.inlay_hints;
import :feature.navigation;
import :feature.selection_ranges;
import :feature.semantic_tokens;
import :feature.signature_help;
import :index.indexer;
import :index.instantiation_shard;
import :index.query;
import :index.shard;
import :index.tu_index;
import :project.command_resolver;
import :project.configuration;
import :project.load;
import :project.project;
import :semantic.symbol;
import :support.bytes;
import :syntax.token;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;

namespace clice::driver {

namespace {

/// What every feature takes.
struct Options {
    DecoInput(meta_var = "<PATH>", help = "The source file", required = false)
    <std::vector<std::string>> path;

    DecoKV(help = R"(JSON file of the markers in the inspected source file, at byte )"
                  R"(offsets: {"points": {"name": 12}, "ranges": {"name": )"
                  R"({"begin": 3, "end": 7}}}; results are keyed by these names)",
           required = false)
    <std::string> markers;

    DecoKV(help =
               "Configuration overlay as a JSON object of clice.toml "
               "sections, over the workspace's configuration",
           required = false)
    <std::string> config_overlay;

    ConfigurationOption configuration;
};

struct FeatureOptions {
    kota::deco::decl::HelpOption help;
    Options inspect;
    LogLevelOption log;
};

/// The options of a feature that checks its own results.
struct CheckedFeatureOptions {
    kota::deco::decl::HelpOption help;
    Options inspect;

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
struct JsonConfig {
    constexpr static auto enum_repr = codec::enum_repr::String;
    constexpr static auto invalid_utf8 = codec::invalid_utf8::Replace;
};

struct FileEntry {
    /// The whole-document feature payload, absent when compilation failed
    /// or the feature is marker-driven.
    std::optional<codec::RawValue> result;

    /// Marker-driven payloads, keyed by the names --markers gives: position
    /// features run once per point, range features once per range. A null
    /// value records a position with no result (e.g. hover on whitespace).
    std::optional<std::map<std::string, codec::RawValue>> markers;

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
struct Output {
    std::string feature;
    std::map<std::string, FileEntry> files;
};

template <typename T>
std::optional<codec::RawValue> to_raw_json(const T& value) {
    auto json = json::to_string<JsonConfig>(value);
    if(!json) {
        LOG_ERROR("serialization failed: {}", json.error().message);
        return std::nullopt;
    }
    return codec::RawValue{std::move(*json)};
}

/// Marker payload for hover: the feature-layer reply before the edge —
/// markdown produced by the same rendering code the server uses, with the
/// symbol range still in byte offsets.
struct HoverResult {
    std::optional<LocalSourceRange> range;
    protocol::MarkupKind kind;
    std::string contents;
};

/// Strict decode for --config-overlay and --markers: an unknown key is a typo,
/// not something to silently ignore.
struct StrictJson {
    constexpr static bool deny_unknown_fields = true;
};

/// What a feature is asked about in the inspected file.
enum class Shape : std::uint8_t {
    /// The whole file.
    File,
    /// Each point --markers gives; without one there is nothing to answer.
    Points,
    /// Each range --markers gives, else the whole file.
    Ranges,
    /// Each range, and each point as an empty one; without either there is
    /// nothing to answer.
    Selections,
};

/// What one run of a feature is given.
struct FeatureParams {
    CompilationUnit& unit;

    /// What `unit` compiled from, for a feature that compiles on its own.
    const CompilationInput& input;

    const Config& config;

    /// The whole file, a marker's range, or a point as an empty range.
    LocalSourceRange range;
};

/// A feature: its operation and line of help, what it is asked about, and
/// what --check runs instead, for one that checks its own results. `run`
/// gives nullopt when its answer does not serialize.
struct FeatureSpec {
    llvm::StringRef name;
    llvm::StringRef description;
    Shape shape = Shape::File;
    std::optional<codec::RawValue> (*run)(const FeatureParams&) = nullptr;
    const FeatureSpec* check = nullptr;
};

std::optional<codec::RawValue> run_folding_ranges(const FeatureParams& params) {
    return to_raw_json(feature::folding_ranges(params.unit));
}

std::optional<codec::RawValue> run_semantic_tokens(const FeatureParams& params) {
    const auto& options = params.config.semantic_tokens;
    return to_raw_json(feature::named_tokens(feature::semantic_tokens(params.unit, options),
                                             feature::semantic_token_legend(options)));
}

std::optional<codec::RawValue> run_semantic_tokens_check(const FeatureParams& params) {
    return to_raw_json(feature::check_semantic_tokens(params.unit, params.config.semantic_tokens));
}

std::optional<codec::RawValue> run_document_symbols(const FeatureParams& params) {
    return to_raw_json(feature::document_symbols(params.unit, params.config.document_symbols));
}

std::optional<codec::RawValue> run_document_symbol_check(const FeatureParams& params) {
    return to_raw_json(
        feature::check_document_symbols(params.unit, params.config.document_symbols));
}

std::optional<codec::RawValue> run_document_links(const FeatureParams& params) {
    return to_raw_json(feature::document_links(params.unit));
}

struct RawLocation {
    std::string path;
    protocol::Range range;
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
std::optional<codec::RawValue> run_inlay_hints(const FeatureParams& params) {
    auto& unit = params.unit;
    auto hints = feature::inlay_hints(unit, params.range, params.config.inlay_hints);

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

std::optional<codec::RawValue> run_selection_range(const FeatureParams& params) {
    return to_raw_json(feature::selection_ranges(params.unit, {params.range.begin}).front());
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
std::optional<codec::RawValue> run_code_action(const FeatureParams& params) {
    auto& unit = params.unit;
    std::vector<RawCodeAction> out;
    auto path = unit.file_path(unit.focus_file().file());
    for(auto& action: feature::code_actions(unit, params.range)) {
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
                raw.edits =
                    *feature::define_edits(*request, path, unit.focus_file().content(), keep);
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
std::optional<codec::RawValue> run_hover(const FeatureParams& params) {
    const auto& config = params.config;
    auto info =
        feature::hover_info(params.unit, params.range.begin, config.hover, config.documentation);
    if(!info) {
        return codec::RawValue{};
    }
    return to_raw_json(HoverResult{info->symbol_range,
                                   protocol::MarkupKind::Markdown,
                                   info->present().as_markdown()});
}

/// Code completion and signature help drive their own compilation: the
/// offset parameterizes the parse itself, so each point gets a fresh
/// completion compile instead of a query against the shared unit. That unit
/// only serves the clean-code gate — the completion entry points discard
/// their unit, so its diagnostics are the sole health signal.
CompilationParams completion_at(const FeatureParams& params) {
    return {
        .kind = CompilationKind::Completion,
        .input = params.input,
        .completion = {.offset = params.range.begin},
        .skip_bodies = params.config.project.skip_header_bodies,
    };
}

std::optional<codec::RawValue> run_code_completion(const FeatureParams& params) {
    const auto& config = params.config;
    // The replies of an editor client that takes everything an item can carry.
    return to_raw_json(
        feature::code_complete(completion_at(params),
                               config.code_completion,
                               config.documentation,
                               {.snippets = true, .insert_replace = true, .markdown = true})
            .list);
}

std::optional<codec::RawValue> run_signature_help(const FeatureParams& params) {
    const auto& config = params.config;
    return to_raw_json(feature::signature_help(completion_at(params),
                                               config.signature_help,
                                               config.documentation));
}

/// Marker payload for document-highlight: the unit's own TU index stands
/// in for the server's file index, the cursor resolved by the same rule.
std::optional<codec::RawValue> run_document_highlight(const FeatureParams& params) {
    auto offset = params.range.begin;
    auto bytes = index::build_tu_index(params.unit);
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
        return to_raw_json(std::vector<feature::Highlight>{});
    }
    return to_raw_json(
        feature::highlights({&shard},
                            instantiations,
                            named->symbols,
                            [&](index::SymbolHash hash) -> std::optional<SymbolKind> {
                                if(auto symbol = tu.find_symbol(hash)) {
                                    return symbol->kind;
                                }
                                return std::nullopt;
                            }));
}

constexpr FeatureSpec document_symbols_check{.name = "document-symbols",
                                             .run = run_document_symbol_check};
constexpr FeatureSpec semantic_tokens_check{.name = "semantic-tokens",
                                            .run = run_semantic_tokens_check};

constexpr std::array features = {
    FeatureSpec{.name = "code-action",
                .description = "The code actions on a selection",
                .shape = Shape::Selections,
                .run = run_code_action},
    FeatureSpec{.name = "code-completion",
                .description = "The completions at a point",
                .shape = Shape::Points,
                .run = run_code_completion},
    FeatureSpec{.name = "document-highlight",
                .description = "The places naming the symbol at a point",
                .shape = Shape::Points,
                .run = run_document_highlight},
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
    FeatureSpec{.name = "hover",
                .description = "The hover card at a point",
                .shape = Shape::Points,
                .run = run_hover},
    FeatureSpec{.name = "inlay-hint",
                .description = "The inlay hints over a range, else the file",
                .shape = Shape::Ranges,
                .run = run_inlay_hints},
    FeatureSpec{.name = "selection-range",
                .description = "The selection ranges at a point",
                .shape = Shape::Points,
                .run = run_selection_range},
    FeatureSpec{.name = "semantic-tokens",
                .description = "The file's semantic tokens",
                .run = run_semantic_tokens,
                .check = &semantic_tokens_check},
    FeatureSpec{.name = "signature-help",
                .description = "The signature help at a point",
                .shape = Shape::Points,
                .run = run_signature_help},
};

/// The inspected file, with the markers --markers places in it.
struct SourceFile {
    std::string path;
    std::string content;
    FileMarkers markers;
};

/// The C-family source at `path`, with the markers the file `markers`
/// places in it.
auto read_source(const Spelling& path, const std::optional<std::string>& markers)
    -> std::expected<SourceFile, std::string> {
    if(!vfs::exists(path)) {
        return std::unexpected(std::format("no such file or directory: {}", path));
    }
    if(vfs::is_directory(path)) {
        return std::unexpected(std::format("{} is a directory, not a source file", path));
    }
    SourceFile file{.path = path.str()};
    auto name = path::filename(file.path);
    if(is_other_language_path(file.path)) {
        return std::unexpected(std::format("{}: not a C-family source file", name));
    }
    auto buffer = vfs::read(file.path);
    if(!buffer) {
        return std::unexpected(std::format("cannot read {}: {}", path, buffer.error().message()));
    }
    file.content = (*buffer)->getBuffer().str();
    if(!markers) {
        return file;
    }

    auto text = vfs::read(*markers);
    if(!text) {
        return std::unexpected(
            std::format("cannot read --markers {}: {}", *markers, text.error().message()));
    }
    auto& [points, ranges] = file.markers;
    if(auto result = json::from_string<StrictJson>((*text)->getBuffer(), file.markers); !result) {
        return std::unexpected(std::format("invalid --markers: {}", result.error().message));
    }
    if(points.contains("") || ranges.contains("")) {
        return std::unexpected("--markers gives every marker a name");
    }
    // A selection feature answers points and ranges under one name each.
    for(const auto& name: points | views::keys) {
        if(ranges.contains(name)) {
            return std::unexpected(std::format("--markers names a point and a range {}", name));
        }
    }
    auto inside = [&](std::uint32_t offset) {
        return offset <= file.content.size();
    };
    if(!ranges::all_of(points | views::values, inside) ||
       !ranges::all_of(ranges | views::values, [&](const LocalSourceRange& range) {
           return range.begin <= range.end && inside(range.end);
       })) {
        return std::unexpected(std::format("--markers puts a marker outside the text of {}", name));
    }
    return file;
}

/// The workspace the inspected file's directory belongs to: itself or the
/// nearest ancestor holding a configuration file or a compile_commands.json
/// (the upward lookup clangd does), so a nested tree resolves its commands
/// as the server would from the project root; `start` itself when none does.
/// Only the ancestors themselves are checked — scanning their
/// subdirectories would let an unrelated sibling project's database win.
CanonicalPath workspace_of(CanonicalRef start) {
    CanonicalPath workspace = start;
    path::walk_ancestors(start, [&](CanonicalRef dir) {
        bool marked = ranges::any_of(config_file_names,
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

/// The project the file in `directory` belongs to, loaded as `clice serve`
/// loads it: the configuration of the nearest project at or above it with
/// --config-overlay put in before the rules compile, as initializationOptions
/// are; the databases the configuration names, else the ones discovered
/// under its root and those between the root and the file; the dependency
/// graph.
auto load_inspected(Project& project, const Options& opts, const Spelling& directory)
    -> std::expected<void, std::string> {
    auto identity = CanonicalPath(directory);
    auto root = workspace_of(identity);
    project.config = Config::load_from_workspace(root, nullptr, nullptr, /*finalized=*/false);
    if(opts.config_overlay) {
        if(auto result = json::from_string<StrictJson>(*opts.config_overlay, project.config);
           !result) {
            return std::unexpected(
                std::format("invalid --config-overlay: {}", result.error().message));
        }
    }
    project.config.finalize(root);
    project.config.keep_own_cache_dir();
    auto requested = opts.configuration.tag();
    if(auto checked = check_requested_configuration(project.config, requested); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    load_build(project,
               root,
               resolve_configuration(project.config, requested),
               compile_commands_above(identity, root));
    return {};
}

/// clice's build of the precompiled header the command reads, as the server
/// builds it for the editor: under its writer's command the header set holds what the
/// compiler compiled, which the unit's own flags may not (a target reusing
/// another target's PCH). Empty when it does not build, or when it reads
/// `file` itself, whose text its guard would then skip.
std::string build_pch(Project& project,
                      const CommandRef& writer,
                      llvm::StringRef file,
                      llvm::StringRef workspace) {
    auto output = vfs::temp_file("clice-inspect", "pch");
    if(!output) {
        LOG_WARN("cannot create a temporary PCH: {}", output.error().message());
        return {};
    }
    auto argv = project.toolchain.render(writer, project.cdb.spelling(writer.file));
    auto unit = clice::compile({
        .kind = CompilationKind::PrecompiledHeader,
        .input = {.arguments = {argv.begin(), argv.end()},
                  .directory = project.commands.get(writer.command).directory,
                  .workspace = workspace.str()},
        .output_file = *output,
        .skip_bodies = project.config.project.skip_header_bodies,
    });
    // The writer's own source is no dep of its unit, but the PCH holds it.
    auto& files = project.file_table;
    auto self = files.intern(Spelling::absolute(file));
    bool reads = writer.file == self || ranges::any_of(unit->deps(), [&](const DepFile& dep) {
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

/// What the file compiles with in its project: the resolution the server
/// performs for a background compile — the file's entry, a host's command
/// for a header, a rule's default command, the builtin fallback — with the
/// header context it synthesizes and the precompiled header the build
/// declares, built (build_pch).
auto project_input(const SourceFile& file, Project& project, CommandResolver& commands)
    -> CompilationInput {
    CompilationInput input{.workspace = project.config.workspace_root.str()};
    auto path_id = project.file_table.intern(Spelling::absolute(file.path));
    auto resolution = commands.resolve_command(path_id, input.directory, input.arguments);
    input.add_focus(file.path, file.content);
    if(resolution.synthesized) {
        input.add_synthesized(resolution.synthesized->files);
    }
    if(auto writer = commands.pch_unit(path_id, resolution)) {
        if(auto pch = build_pch(project, *writer, file.path, input.workspace); !pch.empty()) {
            input.pch = {std::move(pch), 0};
        }
    }
    return input;
}

/// Run the feature over the file and fill its entry. The compile is one
/// pass, deliberately without the preamble PCH the server uses: a shared
/// snapshot pins that the PCH split does not change feature results, so any
/// divergence between the two paths shows as a snapshot mismatch instead of
/// hiding in the preamble. It skips the function bodies the server's
/// preamble leaves out, and loads the precompiled header the build declares
/// (build_pch). With `diagnostics.clang_tidy` the compile runs the file's
/// clang-tidy configuration as the server's does.
void run_feature(FileEntry& entry,
                 const FeatureSpec& spec,
                 const SourceFile& file,
                 const CompilationInput& input,
                 const Config& config) {
    CompilationParams params{
        .kind = CompilationKind::Content,
        .input = input,
        .skip_bodies = config.project.skip_header_bodies,
    };
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

    auto run = [&](LocalSourceRange range) {
        return spec.run({.unit = *unit, .input = input, .config = config, .range = range});
    };
    const auto& [points, ranges] = file.markers;
    std::vector<std::pair<std::string, LocalSourceRange>> places;
    if(spec.shape == Shape::Ranges || spec.shape == Shape::Selections) {
        places.assign(ranges.begin(), ranges.end());
    }
    if(spec.shape == Shape::Points || spec.shape == Shape::Selections) {
        for(const auto& [name, offset]: points) {
            places.emplace_back(name, LocalSourceRange(offset, offset));
        }
    }
    if(spec.shape == Shape::File || (spec.shape == Shape::Ranges && places.empty())) {
        entry.result = run(LocalSourceRange(0, static_cast<std::uint32_t>(file.content.size())));
        if(!entry.result) {
            entry.error = "serialize_error";
        }
        return;
    }
    if(places.empty()) {
        entry.error = "no_markers";
        return;
    }
    std::map<std::string, codec::RawValue> markers;
    for(const auto& [name, range]: places) {
        auto value = run(range);
        if(!value) {
            entry.error = "serialize_error";
            return;
        }
        markers.emplace(name, std::move(*value));
    }
    entry.markers = std::move(markers);
}

int run_inspect(const Options& opts, const FeatureSpec& spec) {
    auto args = positional(opts.path.as_optional(), {"<PATH>"});
    if(!args) {
        return usage_error(Errors::Text, args.error());
    }
    Spelling path(args->front(), Spelling::cwd());
    auto file = read_source(path, opts.markers.as_optional());
    if(!file) {
        LOG_ERROR("{}", file.error());
        return 1;
    }
    FileTable file_table;
    Project project{file_table};
    CommandResolver commands(project);
    if(auto loaded = load_inspected(project, opts, path.parent()); !loaded) {
        LOG_ERROR("{}", loaded.error());
        return 1;
    }

    Output output{.feature = spec.name.str()};
    auto& entry = output.files[path::filename(file->path).str()];
    auto input = project_input(*file, project, commands);
    run_feature(entry, spec, *file, input, project.config);
    if(!input.pch.first.empty()) {
        vfs::remove(input.pch.first);
    }

    auto json = json::to_string<JsonConfig>(output);
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
