module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.command;
import :command.toolchain;
import :compile.compilation;
import :config.config;
import :driver.driver;
import :feature.feature;
import :index.query;
import :index.shard;
import :index.tu_index;
import :project.command_resolver;
import :project.configuration;
import :project.load;
import :project.project;
import :semantic.symbol;
import :vfs.file_system;
import :vfs.path;

namespace clice::driver {

namespace {

struct InspectOptions {
    kota::deco::decl::HelpOption help;

    DecoFlag(names = {"--config-schema"},
             help = "Print the JSON schema of the clice configuration and exit",
             required = false)
    config_schema;

    DecoInput(meta_var = "<FEATURE> <PATH>",
              help =
                  "Feature to run (code_action, code_completion, document_highlight, "
                  "document_links, document_symbol, folding_range, hover, inlay_hint, "
                  "selection_range, semantic_tokens, signature_help) and a "
                  "source file",
              required = false)
    <std::vector<std::string>> inputs;

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
               "Feature options overlay as a JSON object "
               "(only features that take options accept it)",
           required = false)
    <std::string> config;

    DecoKV(help =
               "Build configuration to activate, one of the tags declared on "
               "rules (default: the selected one, else default_configuration)",
           required = false)
    <std::string> configuration;

    LogLevelOption log{.log_level = LogLevel::Warn};
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

/// Strict decode for --config and --markers: an unknown key is a typo,
/// not something to silently ignore.
struct StrictJson {
    constexpr static bool deny_unknown_fields = true;
};

/// The --config JSON overlaid on the feature's default options.
/// The options struct doubles as its config section (all fields
/// `defaulted = true`), so decoding onto a fresh value IS the overlay:
/// missing keys keep the field initializers, exactly like the server's
/// config sections. Runners re-parse on each call; --config was validated up
/// front in run_inspect, so their parse cannot fail.
template <typename Options>
std::optional<Options> parse_feature_config(llvm::StringRef config) {
    Options options;
    if(config.empty()) {
        return options;
    }
    if(auto result = kota::codec::json::from_string<StrictJson>(config, options); !result) {
        LOG_ERROR("invalid --config: {}", result.error().message);
        return std::nullopt;
    }
    return options;
}

std::optional<kota::codec::RawValue> run_folding_ranges(CompilationUnitRef unit,
                                                        [[maybe_unused]] llvm::StringRef config) {
    return to_raw_json(feature::folding_ranges(unit));
}

std::optional<kota::codec::RawValue> run_semantic_tokens(CompilationUnitRef unit,
                                                         [[maybe_unused]] llvm::StringRef config) {
    return to_raw_json(feature::semantic_tokens(unit));
}

std::optional<kota::codec::RawValue> run_document_symbols(CompilationUnitRef unit,
                                                          [[maybe_unused]] llvm::StringRef config) {
    return to_raw_json(feature::document_symbols(unit));
}

std::optional<kota::codec::RawValue> run_document_links(CompilationUnitRef unit,
                                                        [[maybe_unused]] llvm::StringRef config) {
    return to_raw_json(feature::document_links(unit));
}

struct RawLocation {
    std::string path;
    kota::ipc::protocol::Range range;
};

struct RawInlayHintPart {
    std::string value;
    /// Where the piece links, for one naming a symbol the index places or
    /// a target in the main file.
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
/// the server's — a declaration, else the definition, the main file's shard
/// first as an open buffer's is.
std::optional<kota::codec::RawValue> run_inlay_hints(CompilationUnitRef unit,
                                                     LocalSourceRange range,
                                                     llvm::StringRef config) {
    auto hints = feature::inlay_hints(unit,
                                      range,
                                      *parse_feature_config<feature::InlayHintsOptions>(config));

    std::string bytes;
    index::TUIndex tu;
    auto place = [&](index::SymbolHash symbol) -> std::optional<RawLocation> {
        if(bytes.empty()) {
            bytes = index::build_tu_index(unit, {.only_instantiation = true});
            tu = index::TUIndex::from_bytes(bytes);
        }
        auto main = tu.path_count() - 1;
        for(auto kind: {RelationKind::Declaration, RelationKind::Definition}) {
            for(std::uint32_t i = 0; i < tu.path_count(); i += 1) {
                auto path_id = i == 0 ? main : i - 1;
                const auto& shard = tu.shard_of(path_id);
                std::optional<LocalSourceRange> found;
                shard.lookup(symbol, kind, [&](const index::Relation& relation) {
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
                auto position = unit.positions().position(*part.target, PositionEncoding::UTF16);
                raw.label.back().location = RawLocation{
                    .path = unit.file_path(unit.main_file()).str(),
                    .range = {*position, *position},
                };
            }
        }
        out.push_back(std::move(raw));
    }
    return to_raw_json(out);
}

std::optional<kota::codec::RawValue> run_selection_range(CompilationUnitRef unit,
                                                         std::uint32_t offset,
                                                         [[maybe_unused]] llvm::StringRef config) {
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
/// the main file are all kept, no header declares any name.
std::optional<kota::codec::RawValue> run_code_action(CompilationUnitRef unit,
                                                     LocalSourceRange selection,
                                                     [[maybe_unused]] llvm::StringRef config) {
    std::vector<RawCodeAction> out;
    auto path = unit.file_path(unit.main_file());
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
                    unit.main_content(),
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
/// and records a marker with no hover.
std::optional<kota::codec::RawValue> run_hover(CompilationUnitRef unit,
                                               std::uint32_t offset,
                                               llvm::StringRef config) {
    auto options = *parse_feature_config<feature::HoverOptions>(config);
    auto info = feature::hover_info(unit, offset, options);
    if(!info) {
        return kota::codec::RawValue{};
    }
    auto document = info->present();
    auto result = options.parse_comment_as_markdown
                      ? HoverResult{info->symbol_range,
                                    feature::protocol::MarkupKind::Markdown,
                                    document.as_markdown()}
                      : HoverResult{info->symbol_range,
                                    feature::protocol::MarkupKind::PlainText,
                                    document.as_plain_text()};
    return to_raw_json(result);
}

/// Completion-shaped features (code completion, signature help) drive
/// their own compilation: the offset parameterizes the parse itself, so
/// each point gets a fresh completion compile instead of a query
/// against one shared unit. `params` arrives fully configured except for
/// the completion offset.
std::optional<kota::codec::RawValue> run_code_completion(CompilationParams& params,
                                                         llvm::StringRef config) {
    // The replies of an editor client that takes everything an item can carry.
    return to_raw_json(
        feature::code_complete(params,
                               *parse_feature_config<feature::CodeCompletionOptions>(config),
                               {.snippets = true, .insert_replace = true}));
}

std::optional<kota::codec::RawValue> run_signature_help(CompilationParams& params,
                                                        llvm::StringRef config) {
    return to_raw_json(
        feature::signature_help(params,
                                *parse_feature_config<feature::SignatureHelpOptions>(config)));
}

/// Marker payload for document_highlight: the unit's own TU index stands
/// in for the server's file index, the cursor resolved by the same rule.
std::optional<kota::codec::RawValue>
    run_document_highlight(CompilationUnitRef unit,
                           std::uint32_t offset,
                           [[maybe_unused]] llvm::StringRef config) {
    auto bytes = index::build_tu_index(unit, {.only_instantiation = true});
    auto tu = index::TUIndex::from_bytes(bytes);
    const index::Shard& shard = tu.shard_of(tu.path_count() - 1);
    auto named = index::named_at(shard, offset, [&](index::SymbolHash hash) {
        auto symbol = tu.find_symbol(hash);
        return symbol && symbol->kind == SymbolKind::Macro;
    });
    if(!named) {
        return to_raw_json(std::vector<index::Highlight>{});
    }
    return to_raw_json(index::highlights({&shard}, named->symbols));
}

/// A feature runs in exactly one shape: whole-document (`run`), once per
/// point against a shared unit (`run_at`), once per range with a
/// whole-document default (`run_over`), once per point (an empty
/// selection) and per range with no default (`run_select`), or once per
/// point with its own completion compile (`run_complete`).
struct FeatureSpec {
    llvm::StringRef name;
    std::optional<kota::codec::RawValue> (*run)(CompilationUnitRef, llvm::StringRef) = nullptr;
    std::optional<kota::codec::RawValue> (*run_at)(CompilationUnitRef,
                                                   std::uint32_t,
                                                   llvm::StringRef) = nullptr;
    std::optional<kota::codec::RawValue> (*run_over)(CompilationUnitRef,
                                                     LocalSourceRange,
                                                     llvm::StringRef) = nullptr;
    std::optional<kota::codec::RawValue> (*run_select)(CompilationUnitRef,
                                                       LocalSourceRange,
                                                       llvm::StringRef) = nullptr;
    std::optional<kota::codec::RawValue> (*run_complete)(CompilationParams&,
                                                         llvm::StringRef) = nullptr;
    /// Validates --config JSON for the feature; null for features without
    /// options.
    bool (*check_config)(llvm::StringRef) = nullptr;
};

template <typename Options>
bool check_feature_config(llvm::StringRef config) {
    return parse_feature_config<Options>(config).has_value();
}

constexpr std::array features = {
    FeatureSpec{.name = "code_action", .run_select = run_code_action},
    FeatureSpec{.name = "code_completion",
                .run_complete = run_code_completion,
                .check_config = check_feature_config<feature::CodeCompletionOptions>},
    FeatureSpec{.name = "document_highlight", .run_at = run_document_highlight},
    FeatureSpec{.name = "document_links", .run = run_document_links},
    FeatureSpec{.name = "document_symbol", .run = run_document_symbols},
    FeatureSpec{.name = "folding_range", .run = run_folding_ranges},
    FeatureSpec{.name = "hover",
                .run_at = run_hover,
                .check_config = check_feature_config<feature::HoverOptions>},
    FeatureSpec{.name = "inlay_hint",
                .run_over = run_inlay_hints,
                .check_config = check_feature_config<feature::InlayHintsOptions>},
    FeatureSpec{.name = "selection_range", .run_at = run_selection_range},
    FeatureSpec{.name = "semantic_tokens", .run = run_semantic_tokens},
    FeatureSpec{.name = "signature_help",
                .run_complete = run_signature_help,
                .check_config = check_feature_config<feature::SignatureHelpOptions>},
};

const FeatureSpec* find_feature(llvm::StringRef name) {
    auto it = std::ranges::find(features, name, &FeatureSpec::name);
    return it != features.end() ? &*it : nullptr;
}

/// The inspected file, with the markers --markers places in it.
struct SourceFile {
    std::string path;
    std::string content;
    FileMarkers markers;
};

/// The compile command for one file, arguments owned as strings so they
/// outlive the compiles they parameterize.
struct FileCommand {
    std::vector<std::string> arguments;
    std::string directory;
    std::string workspace;
    std::shared_ptr<const SynthesizedContext> synthesized;
};

void apply_command(CompilationParams& params, const FileCommand& command) {
    for(auto& arg: command.arguments) {
        params.arguments.push_back(arg.c_str());
    }
    params.directory = command.directory;
    params.workspace = command.workspace;
    if(command.synthesized) {
        params.add_synthesized(command.synthesized->files);
    }
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
                                        FileTable& files,
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

    command.synthesized = commands
                              .resolve_command(files.intern(Spelling::absolute(file)),
                                               command.directory,
                                               command.arguments)
                              .synthesized;
    return command;
}

/// Run the feature over the file and fill its entry. The compile is one
/// pass, deliberately without the preamble PCH the server uses: a shared
/// snapshot pins that the PCH split does not change feature results, so any
/// divergence between the two paths shows as a snapshot mismatch instead of
/// hiding in the preamble. With `clang_tidy` the compile runs the file's
/// clang-tidy configuration as the server's does.
void run_feature(FileEntry& entry,
                 const FeatureSpec& spec,
                 const SourceFile& file,
                 const FileCommand& command,
                 llvm::StringRef config,
                 bool clang_tidy) {
    auto prepare = [&](CompilationParams& params) {
        apply_command(params, command);
        std::string text = file.content;
        if(command.synthesized) {
            command.synthesized->append_suffix_include(text);
        }
        params.add_remapped_file(file.path, text);
    };

    CompilationParams params;
    params.kind = CompilationKind::Content;
    prepare(params);
    if(clang_tidy) {
        auto resolution = tidy::resolve_tidy_params(file.path);
        if(!resolution.error.empty()) {
            LOG_WARN("{}", resolution.error);
        }
        params.tidy = std::move(resolution.params);
    }

    auto unit = clice::compile(params);
    if(!unit.completed()) {
        entry.error = "compile_error";
        entry.diagnostics = std::ranges::to<std::vector>(
            unit.diagnostics() | std::views::transform(&Diagnostic::message));
        return;
    }

    // The AST builds even for broken sources (a language server must keep
    // working on them), so error diagnostics are reported separately: the
    // test harness rejects code that silently broke instead of pinning
    // garbage.
    if(auto errors = collect_errors(unit); !errors.empty()) {
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
            CompilationParams cp;
            cp.kind = CompilationKind::Completion;
            prepare(cp);
            cp.completion = {file.path, offset};
            auto value = spec.run_complete(cp, config);
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
        entry.result = spec.run(unit, config);
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
            auto value = spec.run_at(unit, offset, config);
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
        entry.result =
            run(unit, LocalSourceRange(0, static_cast<std::uint32_t>(file.content.size())), config);
        if(!entry.result.has_value()) {
            entry.error = "serialize_error";
        }
        return;
    }
    std::map<std::string, kota::codec::RawValue> markers;
    for(const auto& [name, range]: places) {
        auto value = run(unit, range, config);
        if(!value.has_value()) {
            entry.error = "serialize_error";
            return;
        }
        markers.emplace(name, std::move(*value));
    }
    entry.markers = std::move(markers);
}

int run_inspect(const InspectOptions& opts) {
    auto& inputs = *opts.inputs;
    llvm::StringRef feature = inputs[0];
    const auto* spec = find_feature(feature);
    if(spec == nullptr) {
        LOG_ERROR("unknown feature '{}', valid: {}",
                  feature,
                  features | std::views::transform(&FeatureSpec::name));
        return 1;
    }

    // Validate --config up front so a typo fails the whole run with a
    // clear message instead of showing as a feature error.
    llvm::StringRef config = opts.config.has_value() ? llvm::StringRef(*opts.config) : "";
    if(!config.empty()) {
        if(spec->check_config == nullptr) {
            LOG_ERROR("feature '{}' does not accept --config", feature);
            return 1;
        }
        if(!spec->check_config(config)) {
            return 1;
        }
    }

    Spelling abs_path(inputs[1], Spelling::cwd());
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
    if(!flags.empty() && opts.configuration.has_value()) {
        LOG_ERROR("--configuration selects among the workspace's rules; --flags replaces them");
        return 1;
    }
    if(flags.empty()) {
        auto identity = CanonicalPath(directory);
        auto root = workspace_of(identity);
        project.config = Config::load_from_workspace(root);
        auto requested = opts.configuration.value_or("");
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
    }

    auto name = path::filename(file.path).str();
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
    output.feature = feature.str();
    auto& entry = output.files[name];
    auto command = file_command(entry, file.path, flags, directory, file_table, commands);
    if(command) {
        command->workspace = project.config.workspace_root.str();
        run_feature(entry, *spec, file, *command, config, project.config.diagnostics.clang_tidy);
    }

    auto json = kota::codec::json::to_string<InspectJsonConfig>(output);
    if(!json) {
        LOG_ERROR("serialization failed: {}", json.error().message);
        return 1;
    }
    driver::println("{}", *json);
    return 0;
}

auto make_command() {
    return kota::deco::cli::command<InspectOptions>("clice inspect <feature> <path> [OPTIONS]");
}

CommandHandler build(const char*) {
    auto cmd = make_command();
    cmd.match_all([](InspectOptions opts) {
        // A mode flag like --help: ignores feature/path inputs.
        if(opts.config_schema) {
            auto schema = Config::json_schema();
            if(!schema) {
                LOG_ERROR("config schema generation failed: {}", schema.error());
                return 1;
            }
            driver::println("{}", *schema);
            return 0;
        }
        opts.log.apply();
        logging::stderr_logger("inspect", logging::options);
        if(!opts.inputs.has_value() || opts.inputs->size() != 2) {
            auto help = make_command();
            print_usage(help);
            return 1;
        }
        return run_inspect(opts);
    });
    return cmd;
}

[[maybe_unused]] const bool registered =
    register_command(Command::inspect,
                     {.description = "Run a feature on a source file and print raw results as JSON",
                      .build = build});

}  // namespace

}  // namespace clice::driver
