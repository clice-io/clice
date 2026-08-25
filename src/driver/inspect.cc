#include <format>
#include <map>
#include <print>
#include <ranges>

#include "command/argument_parser.h"
#include "command/command.h"
#include "command/toolchain.h"
#include "compile/compilation.h"
#include "driver/driver.h"
#include "feature/feature.h"
#include "index/shard.h"
#include "index/tu_index.h"
#include "server/state/config.h"
#include "support/filesystem.h"
#include "syntax/annotation.h"
#include "syntax/scan.h"

#include "kota/codec/json/json.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "clang/Driver/Types.h"

namespace kota::codec {

/// SymbolKind is a struct wrapping its enum for implicit conversions, so
/// reflection would serialize it as `{"kind_value": ...}`; emit the enum
/// name instead, matching how plain enums serialize under enum_repr::String.
template <typename Config>
struct serialize_visit<json::ValueWriter, clice::SymbolKind, Config> {
    static bool visit(json::ValueWriter& vis, const clice::SymbolKind& kind) {
        return vis.visit_str(
            kota::meta::enum_name(static_cast<clice::SymbolKind::Kind>(kind), "Invalid"));
    }
};

}  // namespace kota::codec

namespace clice::driver {

namespace {

struct InspectOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

    DecoFlag(names = {"--config-schema"},
             help = "Print the JSON schema of the clice configuration and exit",
             required = false)
    config_schema;

    DecoInput(meta_var = "<FEATURE> <PATH>",
              help =
                  "Feature to run (code_completion, document_links, document_symbol, "
                  "folding_range, hover, inlay_hint, semantic_tokens, signature_help, "
                  "tu_index) and a source file or directory",
              required = false)
    <std::vector<std::string>> inputs;

    DecoFlag(names = {"--annotations"},
             help =
                 "Treat inputs as annotated fixture sources: strip inline "
                 "§-markers before compiling (the snap-test grammar)",
             required = false)
    annotations;

    DecoKVStyled(kota::deco::decl::KVStyle::JoinedOrSeparate,
                 names = {"--flags", "--flags="},
                 help =
                     "Compile flags for the inputs as a JSON string array; "
                     "replaces the compile_commands.json lookup",
                 required = false)
    <std::string> flags;

    DecoKVStyled(kota::deco::decl::KVStyle::JoinedOrSeparate,
                 names = {"--config", "--config="},
                 help =
                     "Feature options overlay as a JSON object "
                     "(only features that take options accept it)",
                 required = false)
    <std::string> config;

    DecoKVStyled(kota::deco::decl::KVStyle::JoinedOrSeparate,
                 names = {"--log-level", "--log-level="},
                 help = "Log level: trace, debug, info, warn, error, off",
                 required = false)
    <std::string> log_level;
};

/// JSON layout of the inspect output. Field names stay snake_case (the
/// project's native spelling) and enums serialize as their C++ value
/// names; the TS side owns any mapping to LSP vocabulary.
struct InspectJsonConfig {
    constexpr static auto enum_repr = kota::codec::enum_repr::String;
};

struct FileEntry {
    /// SHA-256 hex of the annotation-stripped content all result offsets
    /// refer to. The TS driver strips with its twin parser and must arrive
    /// at the same hash, or the two implementations have drifted.
    std::string stripped_hash;

    /// The whole-document feature payload, absent when compilation failed,
    /// the feature is marker-driven, or the file is a support file of a
    /// directory unit (hashed but not inspected).
    std::optional<kota::codec::RawValue> result;

    /// Marker-driven payloads, keyed by annotation name (`nameless_<i>`
    /// for unnamed markers): position features run once per `§` point,
    /// range features once per `§⟦...⟧` range. A null value records a
    /// position with no result (e.g. hover on whitespace).
    std::optional<std::map<std::string, kota::codec::RawValue>> markers;

    std::optional<std::string> error;
    std::optional<std::vector<std::string>> diagnostics;
};

/// `files` keys are POSIX-style paths relative to the input directory (the
/// bare filename for a single-file input); std::map keeps the order
/// deterministic.
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
    std::string contents;
};

/// Strict decode for --config: an unknown key is a typo in a fixture's
/// meta block, not something to silently ignore.
struct StrictJson {
    constexpr static bool deny_unknown_fields = true;
};

/// The fixture's --config JSON overlaid on the feature's default options.
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

std::optional<kota::codec::RawValue> run_inlay_hints(CompilationUnitRef unit,
                                                     LocalSourceRange range,
                                                     llvm::StringRef config) {
    return to_raw_json(
        feature::inlay_hints(unit,
                             range,
                             *parse_feature_config<feature::InlayHintsOptions>(config)));
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
    HoverResult result{info->symbol_range,
                       options.parse_comment_as_markdown ? document.as_markdown()
                                                         : document.as_plain_text()};
    return to_raw_json(result);
}

/// Completion-shaped features (code completion, signature help) drive
/// their own compilation: the offset parameterizes the parse itself, so
/// each `§` point gets a fresh completion compile instead of a query
/// against one shared unit. `params` arrives fully configured except for
/// the completion offset.
std::optional<kota::codec::RawValue> run_code_completion(CompilationParams& params,
                                                         llvm::StringRef config) {
    return to_raw_json(
        feature::code_complete(params,
                               *parse_feature_config<feature::CodeCompletionOptions>(config)));
}

std::optional<kota::codec::RawValue> run_signature_help(CompilationParams& params,
                                                        [[maybe_unused]] llvm::StringRef config) {
    return to_raw_json(feature::signature_help(params));
}

/// Occurrence dump of the TU index for the compiled file — the
/// inspect-path pin of the index layer. No LSP request carries this
/// shape, so tu_index fixtures are `verify: inspect`.
struct RawOccurrence {
    LocalSourceRange range;
    SymbolKind kind;
    std::vector<std::string> relations;
};

std::optional<kota::codec::RawValue> run_tu_index(CompilationUnitRef unit,
                                                  [[maybe_unused]] llvm::StringRef config) {
    auto envelope = index::build_tu_index(unit);
    auto index = index::TUIndex::from_bytes(envelope);
    const index::Shard& rows = index.shard_of(index.path_count() - 1);

    llvm::DenseMap<index::SymbolHash, std::vector<index::Relation>> relations;
    rows.for_each_relation([&](index::SymbolHash hash, const index::Relation& relation) {
        relations[hash].push_back(relation);
        return true;
    });

    std::vector<RawOccurrence> out;
    rows.for_each_occurrence([&](const index::Occurrence& occurrence) {
        RawOccurrence raw;
        raw.range = occurrence.range;
        auto symbol = index.find_symbol(occurrence.target);
        raw.kind = symbol ? symbol->kind : SymbolKind(SymbolKind::Invalid);
        if(auto found = relations.find(occurrence.target); found != relations.end()) {
            for(const auto& relation: found->second) {
                if(relation.range == occurrence.range) {
                    raw.relations.emplace_back(kota::meta::enum_name(relation.kind, "Invalid"));
                }
            }
        }
        out.push_back(std::move(raw));
        return true;
    });
    return to_raw_json(out);
}

/// A feature runs in exactly one shape: whole-document (`run`), once per
/// `§` point against a shared unit (`run_at`), once per `§⟦...⟧` range
/// with a whole-document default (`run_over`), or once per `§` point with
/// its own completion compile (`run_complete`).
struct FeatureSpec {
    llvm::StringRef name;
    std::optional<kota::codec::RawValue> (*run)(CompilationUnitRef, llvm::StringRef) = nullptr;
    std::optional<kota::codec::RawValue> (*run_at)(CompilationUnitRef,
                                                   std::uint32_t,
                                                   llvm::StringRef) = nullptr;
    std::optional<kota::codec::RawValue> (*run_over)(CompilationUnitRef,
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
    FeatureSpec{.name = "code_completion",
                .run_complete = run_code_completion,
                .check_config = check_feature_config<feature::CodeCompletionOptions>},
    FeatureSpec{.name = "document_links", .run = run_document_links},
    FeatureSpec{.name = "document_symbol", .run = run_document_symbols},
    FeatureSpec{.name = "folding_range", .run = run_folding_ranges},
    FeatureSpec{.name = "hover",
                .run_at = run_hover,
                .check_config = check_feature_config<feature::HoverOptions>},
    FeatureSpec{.name = "inlay_hint",
                .run_over = run_inlay_hints,
                .check_config = check_feature_config<feature::InlayHintsOptions>},
    FeatureSpec{.name = "semantic_tokens", .run = run_semantic_tokens},
    FeatureSpec{.name = "signature_help", .run_complete = run_signature_help},
    FeatureSpec{.name = "tu_index", .run = run_tu_index},
};

const FeatureSpec* find_feature(llvm::StringRef name) {
    auto it = std::ranges::find(features, name, &FeatureSpec::name);
    return it != features.end() ? &*it : nullptr;
}

/// Named markers sorted by name, then unnamed ones as `nameless_<i>` in
/// source order — the key order snapshots render in.
std::vector<std::pair<std::string, std::uint32_t>> marker_points(const AnnotatedSource& source) {
    std::vector<std::pair<std::string, std::uint32_t>> points;
    for(const auto& entry: source.offsets) {
        points.emplace_back(entry.getKey().str(), entry.getValue());
    }
    std::ranges::sort(points);
    for(std::size_t i = 0; i < source.nameless_offsets.size(); ++i) {
        points.emplace_back(std::format("nameless_{}", i), source.nameless_offsets[i]);
    }
    return points;
}

std::vector<std::pair<std::string, LocalSourceRange>> marker_ranges(const AnnotatedSource& source) {
    std::vector<std::pair<std::string, LocalSourceRange>> ranges;
    for(const auto& entry: source.ranges) {
        // The single allowed nameless range is stored under the empty key.
        auto name = entry.getKey().empty() ? std::string("nameless_0") : entry.getKey().str();
        ranges.emplace_back(std::move(name), entry.getValue());
    }
    std::ranges::sort(ranges, {}, [](const auto& pair) { return pair.first; });
    return ranges;
}

std::string sha256_hex(llvm::StringRef content) {
    auto digest = llvm::SHA256::hash(
        llvm::ArrayRef(reinterpret_cast<const std::uint8_t*>(content.data()), content.size()));
    return llvm::toHex(digest, /*LowerCase=*/true);
}

/// Nearest compile_commands.json from `start` upwards, like clangd.
std::optional<std::string> find_cdb(llvm::StringRef start) {
    llvm::SmallString<256> dir(start);
    while(!dir.empty()) {
        llvm::SmallString<256> candidate(dir);
        path::append(candidate, "compile_commands.json");
        if(fs::exists(candidate)) {
            return std::string(candidate);
        }
        // parent_path returns a prefix into dir's own buffer; truncate in
        // place instead of assign, which trips the SmallVector
        // self-reference assert in Debug LLVM.
        llvm::StringRef parent = path::parent_path(dir);
        if(parent.size() == dir.size()) {
            break;
        }
        dir.truncate(parent.size());
    }
    return std::nullopt;
}

std::vector<std::string> error_messages(CompilationUnit& unit, bool errors_only = false) {
    auto messages = unit.diagnostics() | std::views::filter([&](const Diagnostic& diagnostic) {
                        return !errors_only || diagnostic.id.level >= DiagnosticLevel::Error;
                    }) |
                    std::views::transform(&Diagnostic::message);
    return std::ranges::to<std::vector>(messages);
}

/// One source file of the inspected input, stripped and ready to compile.
struct SourceFile {
    /// POSIX-style path relative to the input directory (the bare filename
    /// for a single-file input) — the key of the file's output entry.
    std::string rel;
    std::string abs;
    AnnotatedSource source;
    /// Module declaration facts from the dependency scan (directory mode).
    ScanResult scan;
    /// The compile_commands.json governing the file, empty when none (and
    /// in the explicit-flags channel). Module discovery and PCM attachment
    /// are scoped per database, so nested projects that both declare a
    /// module named `core` don't collide in one namespace.
    std::string project;
};

/// The compile command for one file, arguments owned as strings so they
/// outlive the compiles they parameterize.
struct FileCommand {
    std::vector<std::string> arguments;
    std::string directory;
};

void apply_command(CompilationParams& params, const FileCommand& command) {
    for(auto& arg: command.arguments) {
        params.arguments.push_back(arg.c_str());
    }
    params.directory = command.directory;
}

clang::driver::types::ID file_type(llvm::StringRef file) {
    namespace types = clang::driver::types;
    auto ext = path::extension(file);
    return ext.empty() ? types::TY_INVALID
                       : types::lookupTypeForExtension(llvm::StringRef(ext).drop_front());
}

bool is_header_type(clang::driver::types::ID type) {
    namespace types = clang::driver::types;
    return type == types::TY_CHeader || type == types::TY_CXXHeader;
}

/// The compile command for `file`. Explicit --flag arguments (the snap-test
/// channel — the harness owns the flags, no compile_commands.json exists)
/// apply uniformly to every input file; otherwise the file's entry in
/// `database`, a language-compatible donor entry for headers, or default
/// flags. On failure records the error on `entry` and returns nullopt.
std::optional<FileCommand> file_command(FileEntry& entry,
                                        const std::string& file,
                                        llvm::ArrayRef<std::string> flags,
                                        llvm::StringRef flags_directory,
                                        CompilationDatabase* database,
                                        Toolchain& toolchain) {
    namespace types = clang::driver::types;
    auto type = file_type(file);
    bool is_header = is_header_type(type);

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
        // input directory so they don't depend on where inspect was launched.
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

    // A header without its own entry borrows the command of the nearest
    // language-compatible translation unit in the database — the server
    // resolves header contexts from host sources the same way, and generic
    // default flags would drop the project's -I/-D/-std. Ranking: a C++
    // donor beats a C one (a C++ header never takes a C command; an
    // ambiguous .h prefers C++ but accepts C in a pure-C project), longest
    // common path prefix breaks ties.
    llvm::StringRef donor;
    if(is_header && database != nullptr && !database->has_entry(file)) {
        std::pair<int, std::size_t> best{-1, 0};
        for(auto& candidate: database->get_entries()) {
            llvm::StringRef donor_path = database->resolve_path(candidate.file);
            auto donor_ext = path::extension(donor_path);
            auto donor_type = donor_ext.empty()
                                  ? types::TY_INVALID
                                  : types::lookupTypeForExtension(donor_ext.drop_front());
            bool donor_cxx = donor_type != types::TY_INVALID && types::isCXX(donor_type);
            if(type == types::TY_CXXHeader && !donor_cxx) {
                continue;
            }
            auto [it, _] = std::ranges::mismatch(donor_path, file);
            std::pair<int, std::size_t> score{donor_cxx ? 1 : 0,
                                              static_cast<std::size_t>(it - donor_path.begin())};
            if(donor.empty() || score > best) {
                best = score;
                donor = donor_path;
            }
        }
    }

    // lookup() synthesizes a default command for unknown files, so an
    // explicit entry check decides between the CDB and our fallback.
    if(database != nullptr && database->has_entry(file)) {
        auto commands = database->lookup(file);
        auto& cdb_command = commands.front();
        toolchain.resolve_or_warn(cdb_command);
        for(const char* arg: cdb_command.to_argv()) {
            command.arguments.emplace_back(arg);
        }
        command.directory = cdb_command.resolved.directory.str();
    } else if(!donor.empty()) {
        auto commands = database->lookup(donor);
        auto& cdb_command = commands.front();
        toolchain.resolve_or_warn(cdb_command);
        // The donor's resolved flags (including its -x language) apply to
        // the header itself; to_argv() re-derives -main-file-name from it.
        cdb_command.source_file = file.c_str();
        for(const char* arg: cdb_command.to_argv()) {
            command.arguments.emplace_back(arg);
        }
        command.directory = cdb_command.resolved.directory.str();
    } else {
        // No CDB entry for this file: query the toolchain with default
        // flags. Uncached, but this path only runs for files outside any
        // compilation database. C++ inputs pin the corpus-aligned c++20;
        // other C-family languages keep their driver defaults so a .c or
        // .m file is not misparsed as C++.
        LOG_WARN("no compile command for {}; using default flags", file);
        std::vector<const char*> driver_args;
        if(type != types::TY_INVALID && types::isCXX(type)) {
            driver_args = {"clang++", "-std=c++20", "-fsyntax-only", file.c_str()};
        } else if(type == types::TY_CHeader) {
            // An ambiguous header is C++ by default, like clangd; -x forces
            // TU semantics instead of a precompiled-header job.
            driver_args = {"clang++", "-x", "c++", "-std=c++20", "-fsyntax-only", file.c_str()};
        } else {
            driver_args = {"clang", "-fsyntax-only", file.c_str()};
        }
        auto cc1 = Toolchain::query(driver_args, file);
        if(!cc1) {
            entry.error = "toolchain_error";
            entry.diagnostics = {std::move(cc1.error())};
            return std::nullopt;
        }
        command.arguments = std::move(*cc1);
        command.directory = path::parent_path(file).str();
    }
    return command;
}

/// Whether a directory-mode file is inspected. Only applied under
/// --annotations, where a directory input is one fixture unit: a file
/// participates when it carries markers of the feature's shape, plus the
/// unit entry (main.cpp) for whole-document shapes. Everything else is a
/// support file — compiled into participants' units and hashed, but not
/// run. A plain directory inspect runs the feature over every file.
bool participates(const FeatureSpec& spec, const SourceFile& file) {
    bool has_points = !file.source.offsets.empty() || !file.source.nameless_offsets.empty();
    if(spec.run_at != nullptr || spec.run_complete != nullptr) {
        return has_points;
    }
    return file.rel == "main.cpp" || has_points || !file.source.ranges.empty();
}

/// Run the feature over one participating file and fill its entry. The
/// compile is one pass, deliberately without the preamble PCH the server
/// uses: a shared snapshot pins that the PCH split does not change feature
/// results, so any divergence between the two paths surfaces as a snapshot
/// mismatch instead of hiding in the preamble. It sees the whole unit —
/// every stripped file is remapped and every built PCM attached — so
/// cross-file fixtures compile like the server's view of the workspace.
/// With `participant` false only the compile runs: the file's errors still
/// reach the fixture diagnostics gate, but no feature output is produced.
void run_feature(FileEntry& entry,
                 const FeatureSpec& spec,
                 const SourceFile& file,
                 llvm::ArrayRef<SourceFile> sources,
                 const llvm::StringMap<std::string>& pcms,
                 const FileCommand& command,
                 llvm::StringRef config,
                 bool participant) {
    const AnnotatedSource& source = file.source;

    auto prepare = [&](CompilationParams& params) {
        apply_command(params, command);
        for(const auto& sibling: sources) {
            params.add_remapped_file(sibling.abs, sibling.source.content);
        }
        for(const auto& pcm: pcms) {
            // Like the server path, withhold the PCM of the module this
            // file itself declares — attaching it would redeclare the
            // module the compile is defining.
            if(file.scan.is_interface_unit && pcm.getKey() == file.scan.module_name) {
                continue;
            }
            params.pcms.try_emplace(pcm.getKey(), pcm.getValue());
        }
    };

    CompilationParams params;
    params.kind = CompilationKind::Content;
    prepare(params);

    auto unit = clice::compile(params);
    if(!unit.completed()) {
        entry.error = "compile_error";
        entry.diagnostics = error_messages(unit);
        return;
    }

    // The AST builds even for broken sources (a language server must keep
    // working on them), so error diagnostics are surfaced separately: the
    // snap harness rejects fixtures whose code or annotations silently
    // broke instead of pinning garbage.
    if(auto errors = error_messages(unit, /*errors_only=*/true); !errors.empty()) {
        entry.diagnostics = std::move(errors);
    }

    if(!participant) {
        return;
    }

    if(spec.run_complete != nullptr) {
        // Completion shape: the plain compile above only serves the
        // clean-fixture gate — the completion entry points discard their
        // unit, so its diagnostics are the sole health signal. Each `§`
        // point then compiles again with the offset applied.
        auto points = marker_points(source);
        if(points.empty()) {
            entry.error = "no_markers";
            return;
        }
        std::map<std::string, kota::codec::RawValue> markers;
        for(auto& [name, offset]: points) {
            CompilationParams cp;
            cp.kind = CompilationKind::Completion;
            prepare(cp);
            cp.completion = {file.abs, offset};
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
        // Position feature: run once per `§` point. A fixture without any
        // point has nothing to pin — that is a broken fixture, not an
        // empty result.
        auto points = marker_points(source);
        if(points.empty()) {
            entry.error = "no_markers";
            return;
        }
        std::map<std::string, kota::codec::RawValue> markers;
        for(auto& [name, offset]: points) {
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

    // Range feature: run once per `§⟦...⟧` range, or over the whole
    // document when the fixture marks none.
    auto ranges = marker_ranges(source);
    if(ranges.empty()) {
        entry.result =
            spec.run_over(unit,
                          LocalSourceRange(0, static_cast<std::uint32_t>(source.content.size())),
                          config);
        if(!entry.result.has_value()) {
            entry.error = "serialize_error";
        }
        return;
    }
    std::map<std::string, kota::codec::RawValue> markers;
    for(auto& [name, range]: ranges) {
        auto value = spec.run_over(unit, range, config);
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
    // clear message instead of surfacing as a per-file feature error.
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

    llvm::SmallString<256> abs_path(inputs[1]);
    if(auto err = fs::make_absolute(abs_path)) {
        LOG_ERROR("cannot resolve {}: {}", inputs[1], err.message());
        return 1;
    }
    path::remove_dots(abs_path, /*remove_dot_dot=*/true);
    if(!fs::exists(abs_path)) {
        LOG_ERROR("no such file or directory: {}", abs_path);
        return 1;
    }
    bool is_dir = fs::is_directory(abs_path);

    /// (rel key, absolute path) per file, sorted by the map later.
    std::vector<std::pair<std::string, std::string>> files;
    if(is_dir) {
        std::error_code ec;
        for(llvm::sys::fs::recursive_directory_iterator it(abs_path, ec), end; it != end && !ec;
            it.increment(ec)) {
            if(!is_c_family_file(it->path())) {
                continue;
            }
            llvm::StringRef rel = it->path();
            rel.consume_front(abs_path);
            rel.consume_front("/");
            rel.consume_front("\\");
            files.emplace_back(path::convert_to_slash(rel), it->path());
        }
        if(ec) {
            LOG_ERROR("cannot walk {}: {}", abs_path, ec.message());
            return 1;
        }
    } else {
        files.emplace_back(path::filename(abs_path).str(), std::string(abs_path));
    }

    InspectOutput output;
    output.feature = feature.str();

    // Every readable file gets an entry up front: the hash of its stripped
    // content feeds the C++/TS stripper-twin check for support files too,
    // and module/feature errors below land on stable entries.
    std::vector<SourceFile> sources;
    for(auto& [rel, abs]: files) {
        auto buffer = llvm::MemoryBuffer::getFile(abs);
        if(!buffer) {
            FileEntry entry;
            entry.error = "read_error";
            entry.diagnostics = {buffer.getError().message()};
            output.files.emplace(rel, std::move(entry));
            continue;
        }
        // Only fixture sources carry the §-annotation grammar; ordinary
        // code may legitimately contain `§` (in strings or comments) and
        // must reach the compiler verbatim.
        AnnotatedSource source;
        if(opts.annotations) {
            source = AnnotatedSource::from((*buffer)->getBuffer());
        } else {
            source.content = (*buffer)->getBuffer().str();
        }
        FileEntry entry;
        entry.stripped_hash = sha256_hex(source.content);
        output.files.emplace(rel, std::move(entry));
        sources.push_back({rel, abs, std::move(source), {}});
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

    // Each file resolves against the compile_commands.json nearest to it,
    // so a directory spanning nested projects picks up every inner
    // database. Files without a CDB entry fall back to a per-file
    // toolchain query in file_command.
    std::map<std::string, CompilationDatabase> databases;
    auto database_for = [&](llvm::StringRef file) -> CompilationDatabase* {
        auto cdb = find_cdb(path::parent_path(file));
        if(!cdb) {
            return nullptr;
        }
        auto [it, inserted] = databases.try_emplace(*cdb);
        if(inserted && !it->second.load(*cdb)) {
            // Keep the empty entry so the failure is logged once; its files
            // take the default-flags fallback.
            LOG_WARN("failed to load {}", *cdb);
        }
        return &it->second;
    };

    Toolchain toolchain;
    llvm::StringRef unit_directory =
        is_dir ? llvm::StringRef(abs_path) : path::parent_path(abs_path);
    auto command_for = [&](FileEntry& entry, const SourceFile& file) {
        return file_command(entry,
                            file.abs,
                            flags,
                            unit_directory,
                            flags.empty() ? database_for(file.abs) : nullptr,
                            toolchain);
    };

    // Serial module builder (directory mode): scan for module declarations
    // and build each interface unit's PCM in dependency order, so importing
    // files in the unit compile like they do against the server's module
    // pipeline. A dependency cycle leaves its modules unbuilt and surfaces
    // as ordinary compile errors on the importers.
    std::map<std::string, llvm::StringMap<std::string>> pcms;
    std::vector<std::string> pcm_files;
    if(is_dir) {
        bool has_modules = false;
        for(auto& source: sources) {
            source.scan = scan_quick(source.source.content);
            if(flags.empty()) {
                source.project = find_cdb(path::parent_path(source.abs)).value_or("");
            }
            has_modules |= source.scan.is_interface_unit || source.scan.need_preprocess;
        }

        std::map<std::string, llvm::StringMap<SourceFile*>> interfaces;
        if(has_modules) {
            // Preprocessing scans run over the stripped unit through an
            // in-memory overlay.
            auto memory = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
            for(const auto& source: sources) {
                memory->addFile(source.abs,
                                0,
                                llvm::MemoryBuffer::getMemBufferCopy(source.source.content));
            }
            auto overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
                llvm::vfs::getRealFileSystem());
            overlay->pushOverlay(memory);

            SharedScanCache cache;
            auto scan_with = [&](SourceFile& source, auto scan) -> std::optional<ScanResult> {
                auto command = command_for(output.files.find(source.rel)->second, source);
                if(!command) {
                    return std::nullopt;
                }
                std::vector<const char*> argv;
                for(auto& arg: command->arguments) {
                    argv.push_back(arg.c_str());
                }
                return scan(argv, command->directory, {}, &cache, overlay);
            };

            // A module declaration behind #if/#ifdef is invisible to the
            // quick scan (need_preprocess); evaluate the conditionals to
            // learn whether the file really declares an interface.
            for(auto& source: sources) {
                if(!source.scan.need_preprocess) {
                    continue;
                }
                if(auto result = scan_with(source, scan_module_decl)) {
                    source.scan.module_name = std::move(result->module_name);
                    source.scan.is_interface_unit = result->is_interface_unit;
                }
            }

            for(auto& source: sources) {
                if(!source.scan.is_interface_unit || source.scan.module_name.empty()) {
                    continue;
                }
                auto [it, inserted] =
                    interfaces[source.project].try_emplace(source.scan.module_name, &source);
                if(!inserted) {
                    output.files.find(source.rel)->second.error = "duplicate_module";
                }
            }

            // The quick scan only detects module declarations; imports can
            // be macro-formed, so dependency edges come from the
            // preprocessing scan.
            for(const auto& [project, group]: interfaces) {
                for(const auto& entry: group) {
                    SourceFile& source = *entry.second;
                    if(auto result = scan_with(source, scan_precise)) {
                        source.scan.modules = std::move(result->modules);
                    }
                }
            }
        }

        for(const auto& [project, group]: interfaces) {
            llvm::StringMap<std::string>& project_pcms = pcms[project];
            llvm::StringSet<> visited;
            auto build = [&](auto&& self, llvm::StringRef name) -> void {
                if(!visited.insert(name).second) {
                    return;
                }
                SourceFile& source = *group.find(name)->second;
                for(auto& dep: source.scan.modules) {
                    if(group.contains(dep)) {
                        self(self, dep);
                    }
                }

                FileEntry& entry = output.files.find(source.rel)->second;
                auto command = command_for(entry, source);
                if(!command) {
                    return;
                }
                auto tmp = fs::createTemporaryFile("clice-pcm", "pcm");
                if(!tmp) {
                    entry.error = "module_error";
                    entry.diagnostics = {"failed to create temporary PCM file"};
                    return;
                }
                pcm_files.push_back(*tmp);

                CompilationParams params;
                params.kind = CompilationKind::ModuleInterface;
                params.output_file = *tmp;
                apply_command(params, *command);
                for(const auto& sibling: sources) {
                    params.add_remapped_file(sibling.abs, sibling.source.content);
                }
                for(const auto& pcm: project_pcms) {
                    params.pcms.try_emplace(pcm.getKey(), pcm.getValue());
                }

                PCMInfo info;
                auto unit = clice::compile(params, info);
                if(!unit.completed()) {
                    entry.error = "module_error";
                    entry.diagnostics = error_messages(unit);
                    return;
                }
                project_pcms.try_emplace(name, *tmp);
            };
            for(const auto& entry: group) {
                build(build, entry.getKey());
            }
        }
    }

    for(auto& source: sources) {
        bool participant = !(is_dir && opts.annotations) || participates(*spec, source);
        // Non-participating siblings still health-compile so their errors
        // reach the fixture diagnostics gate, like the server path opening
        // every sibling — except headers, which may be valid only through
        // their includer and never compile standalone on either path.
        if(!participant && is_header_type(file_type(source.abs))) {
            continue;
        }
        FileEntry& entry = output.files.find(source.rel)->second;
        auto command = command_for(entry, source);
        if(!command) {
            continue;
        }
        run_feature(entry,
                    *spec,
                    source,
                    sources,
                    pcms[source.project],
                    *command,
                    config,
                    participant);
    }

    for(auto& path: pcm_files) {
        fs::remove(path);
    }

    auto json = kota::codec::json::to_string<InspectJsonConfig>(output);
    if(!json) {
        LOG_ERROR("serialization failed: {}", json.error().message);
        return 1;
    }
    std::println("{}", *json);
    return 0;
}

auto make_command() {
    return kota::deco::cli::command<InspectOptions>("clice inspect <feature> <path> [OPTIONS]");
}

}  // namespace

void add_inspect(kota::deco::cli::SubCommander& root, int& exit_code) {
    auto cmd = make_command();
    cmd.matchAll([&exit_code](InspectOptions opts) {
           if(opts.help) {
               auto help = make_command();
               print_usage(help);
               exit_code = 0;
               return;
           }
           // A mode flag like --help: ignores feature/path inputs.
           if(opts.config_schema) {
               auto schema = Config::json_schema();
               if(!schema) {
                   LOG_ERROR("config schema generation failed: {}", schema.error());
                   return;
               }
               std::println("{}", *schema);
               exit_code = 0;
               return;
           }
           if(!apply_log_level(opts.log_level.value_or("warn"))) {
               return;
           }
           logging::stderr_logger("inspect", logging::options);
           if(!opts.inputs.has_value() || opts.inputs->size() != 2) {
               auto help = make_command();
               print_usage(help);
               return;
           }
           exit_code = run_inspect(opts);
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "inspect",
              .description = "Run a feature on source files and print raw results as JSON"},
             std::move(cmd));
}

}  // namespace clice::driver
