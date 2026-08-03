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
#include "support/filesystem.h"
#include "syntax/annotation.h"

#include "kota/codec/json/json.h"
#include "llvm/Support/SHA256.h"
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

    DecoInput(
        meta_var = "<FEATURE> <PATH>",
        help =
            "Feature to run (code_completion, document_links, document_symbol, " "folding_range, hover, inlay_hint, semantic_tokens, signature_help) " "and a source file or directory",
        required = false)
    <std::vector<std::string>> inputs;

    DecoFlag(
        names = {"--annotations"},
        help =
            "Treat inputs as annotated fixture sources: strip inline " "§-markers before compiling (the snap-test grammar)",
        required = false)
    annotations;

    DecoKVStyled(
        kota::deco::decl::KVStyle::JoinedOrSeparate,
        names = {"--config", "--config="},
        help =
            "Feature options overlay as a JSON object " "(only features that take options accept it)",
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

    /// The whole-document feature payload, absent when compilation failed
    /// or the feature is marker-driven.
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

std::optional<kota::codec::RawValue> run_folding_ranges(CompilationUnitRef unit) {
    return to_raw_json(feature::folding_ranges(unit));
}

std::optional<kota::codec::RawValue> run_semantic_tokens(CompilationUnitRef unit) {
    return to_raw_json(feature::semantic_tokens(unit));
}

std::optional<kota::codec::RawValue> run_document_symbols(CompilationUnitRef unit) {
    return to_raw_json(feature::document_symbols(unit));
}

std::optional<kota::codec::RawValue> run_document_links(CompilationUnitRef unit) {
    return to_raw_json(feature::document_links(unit));
}

std::optional<kota::codec::RawValue> run_inlay_hints(CompilationUnitRef unit,
                                                     LocalSourceRange range) {
    return to_raw_json(feature::inlay_hints(unit, range));
}

/// nullopt = serialization failure; an empty RawValue serializes as null
/// and records a marker with no hover.
std::optional<kota::codec::RawValue> run_hover(CompilationUnitRef unit, std::uint32_t offset) {
    auto info = feature::hover_info(unit, offset);
    if(!info) {
        return kota::codec::RawValue{};
    }
    HoverResult result{info->symbol_range, info->present().as_markdown()};
    return to_raw_json(result);
}

/// Strict decode for --config: an unknown key is a typo in a fixture's
/// meta block, not something to silently ignore.
struct StrictJson {
    constexpr static bool deny_unknown_fields = true;
};

/// The fixture's --config JSON overlaid on default completion options.
/// The options struct doubles as its config section (all fields
/// `defaulted`), so decoding onto a fresh value IS the overlay: missing
/// keys keep the field initializers, exactly like the server's
/// [code_completion] section.
std::optional<feature::CodeCompletionOptions> parse_completion_config(llvm::StringRef config) {
    feature::CodeCompletionOptions options;
    if(config.empty()) {
        return options;
    }
    if(auto result = kota::codec::json::from_json<StrictJson>(config, options); !result) {
        LOG_ERROR("invalid --config: {}", result.error().message);
        return std::nullopt;
    }
    return options;
}

/// Completion-shaped features (code completion, signature help) drive
/// their own compilation: the offset parameterizes the parse itself, so
/// each `§` point gets a fresh completion compile instead of a query
/// against one shared unit. `params` arrives fully configured except for
/// the completion offset. `config` was validated up front in run_inspect,
/// so re-parsing here cannot fail.
std::optional<kota::codec::RawValue> run_code_completion(CompilationParams& params,
                                                         llvm::StringRef config) {
    return to_raw_json(feature::code_complete(params, *parse_completion_config(config)));
}

std::optional<kota::codec::RawValue> run_signature_help(CompilationParams& params,
                                                        [[maybe_unused]] llvm::StringRef config) {
    return to_raw_json(feature::signature_help(params));
}

/// A feature runs in exactly one shape: whole-document (`run`), once per
/// `§` point against a shared unit (`run_at`), once per `§⟦...⟧` range
/// with a whole-document default (`run_over`), or once per `§` point with
/// its own completion compile (`run_complete`).
struct FeatureSpec {
    llvm::StringRef name;
    std::optional<kota::codec::RawValue> (*run)(CompilationUnitRef) = nullptr;
    std::optional<kota::codec::RawValue> (*run_at)(CompilationUnitRef, std::uint32_t) = nullptr;
    std::optional<kota::codec::RawValue> (*run_over)(CompilationUnitRef,
                                                     LocalSourceRange) = nullptr;
    std::optional<kota::codec::RawValue> (*run_complete)(CompilationParams&,
                                                         llvm::StringRef) = nullptr;
    /// Whether --config JSON applies to this feature.
    bool accepts_config = false;
};

constexpr std::array features = {
    FeatureSpec{.name = "code_completion",
                .run_complete = run_code_completion,
                .accepts_config = true},
    FeatureSpec{.name = "document_links", .run = run_document_links},
    FeatureSpec{.name = "document_symbol", .run = run_document_symbols},
    FeatureSpec{.name = "folding_range", .run = run_folding_ranges},
    FeatureSpec{.name = "hover", .run_at = run_hover},
    FeatureSpec{.name = "inlay_hint", .run_over = run_inlay_hints},
    FeatureSpec{.name = "semantic_tokens", .run = run_semantic_tokens},
    FeatureSpec{.name = "signature_help", .run_complete = run_signature_help},
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

/// Compile `file` in one pass, deliberately without the preamble PCH the
/// server uses: a shared snapshot pins that the PCH split does not change
/// feature results, so any divergence between the two paths surfaces as a
/// snapshot mismatch instead of hiding in the preamble. Nothing is written
/// to disk. `database` is the one nearest to the file, or null when no
/// compile_commands.json exists above it.
FileEntry process_file(const std::string& file,
                       llvm::StringRef feature,
                       CompilationDatabase* database,
                       Toolchain& toolchain,
                       bool annotations,
                       llvm::StringRef config) {
    FileEntry entry;

    auto buffer = llvm::MemoryBuffer::getFile(file);
    if(!buffer) {
        entry.error = "read_error";
        entry.diagnostics = {buffer.getError().message()};
        return entry;
    }

    // Only fixture sources carry the §-annotation grammar; ordinary code
    // may legitimately contain `§` (in strings or comments) and must reach
    // the compiler verbatim.
    AnnotatedSource source;
    if(annotations) {
        source = AnnotatedSource::from((*buffer)->getBuffer());
    } else {
        source.content = (*buffer)->getBuffer().str();
    }
    entry.stripped_hash = sha256_hex(source.content);

    CompilationParams params;
    params.kind = CompilationKind::Content;

    namespace types = clang::driver::types;
    auto ext = path::extension(file);
    auto type = ext.empty() ? types::TY_INVALID
                            : types::lookupTypeForExtension(llvm::StringRef(ext).drop_front());
    bool is_header = type == types::TY_CHeader || type == types::TY_CXXHeader;

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

    /// Owns the fallback cc1 strings so params.arguments stays valid.
    std::vector<std::string> owned_args;
    // lookup() synthesizes a default command for unknown files, so an
    // explicit entry check decides between the CDB and our fallback.
    if(database != nullptr && database->has_entry(file)) {
        auto commands = database->lookup(file);
        auto& command = commands.front();
        toolchain.resolve_or_warn(command);
        params.arguments = command.to_argv();
        params.directory = command.resolved.directory.str();
    } else if(!donor.empty()) {
        auto commands = database->lookup(donor);
        auto& command = commands.front();
        toolchain.resolve_or_warn(command);
        // The donor's resolved flags (including its -x language) apply to
        // the header itself; to_argv() re-derives -main-file-name from it.
        command.source_file = file.c_str();
        params.arguments = command.to_argv();
        params.directory = command.resolved.directory.str();
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
            return entry;
        }
        owned_args = std::move(*cc1);
        for(auto& arg: owned_args) {
            params.arguments.push_back(arg.c_str());
        }
        params.directory = path::parent_path(file).str();
    }

    params.add_remapped_file(file, source.content);

    auto unit = clice::compile(params);
    if(!unit.completed()) {
        entry.error = "compile_error";
        entry.diagnostics = error_messages(unit);
        return entry;
    }

    // The AST builds even for broken sources (a language server must keep
    // working on them), so error diagnostics are surfaced separately: the
    // snap harness rejects fixtures whose code or annotations silently
    // broke instead of pinning garbage.
    if(auto errors = error_messages(unit, /*errors_only=*/true); !errors.empty()) {
        entry.diagnostics = std::move(errors);
    }

    const auto& spec = *find_feature(feature);

    if(spec.run_complete != nullptr) {
        // Completion shape: the plain compile above only serves the
        // clean-fixture gate — the completion entry points discard their
        // unit, so its diagnostics are the sole health signal. Each `§`
        // point then compiles again with the offset applied; argv pointees
        // are interned in the database (or owned_args), so copying the
        // argument vector into fresh params is safe.
        auto points = marker_points(source);
        if(points.empty()) {
            entry.error = "no_markers";
            return entry;
        }
        std::map<std::string, kota::codec::RawValue> markers;
        for(auto& [name, offset]: points) {
            CompilationParams cp;
            cp.kind = CompilationKind::Completion;
            cp.arguments = params.arguments;
            cp.directory = params.directory;
            cp.completion = {file, offset};
            cp.add_remapped_file(file, source.content);
            auto value = spec.run_complete(cp, config);
            if(!value.has_value()) {
                entry.error = "serialize_error";
                return entry;
            }
            markers.emplace(name, std::move(*value));
        }
        entry.markers = std::move(markers);
        return entry;
    }

    if(spec.run != nullptr) {
        entry.result = spec.run(unit);
        if(!entry.result.has_value()) {
            entry.error = "serialize_error";
        }
        return entry;
    }

    if(spec.run_at != nullptr) {
        // Position feature: run once per `§` point. A fixture without any
        // point has nothing to pin — that is a broken fixture, not an
        // empty result.
        auto points = marker_points(source);
        if(points.empty()) {
            entry.error = "no_markers";
            return entry;
        }
        std::map<std::string, kota::codec::RawValue> markers;
        for(auto& [name, offset]: points) {
            auto value = spec.run_at(unit, offset);
            if(!value.has_value()) {
                entry.error = "serialize_error";
                return entry;
            }
            markers.emplace(name, std::move(*value));
        }
        entry.markers = std::move(markers);
        return entry;
    }

    // Range feature: run once per `§⟦...⟧` range, or over the whole
    // document when the fixture marks none.
    auto ranges = marker_ranges(source);
    if(ranges.empty()) {
        entry.result =
            spec.run_over(unit,
                          LocalSourceRange(0, static_cast<std::uint32_t>(source.content.size())));
        if(!entry.result.has_value()) {
            entry.error = "serialize_error";
        }
        return entry;
    }
    std::map<std::string, kota::codec::RawValue> markers;
    for(auto& [name, range]: ranges) {
        auto value = spec.run_over(unit, range);
        if(!value.has_value()) {
            entry.error = "serialize_error";
            return entry;
        }
        markers.emplace(name, std::move(*value));
    }
    entry.markers = std::move(markers);
    return entry;
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
        if(!spec->accepts_config) {
            LOG_ERROR("feature '{}' does not accept --config", feature);
            return 1;
        }
        if(!parse_completion_config(config)) {
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

    // Each file resolves against the compile_commands.json nearest to it,
    // so a directory spanning nested projects picks up every inner
    // database. Files without a CDB entry fall back to a per-file
    // toolchain query in process_file.
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
    InspectOutput output;
    output.feature = feature.str();
    for(auto& [rel, abs]: files) {
        output.files.emplace(rel,
                             process_file(abs,
                                          feature,
                                          database_for(abs),
                                          toolchain,
                                          static_cast<bool>(opts.annotations),
                                          config));
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
