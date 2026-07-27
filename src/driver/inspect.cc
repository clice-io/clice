#include <map>
#include <print>
#include <ranges>

#include "command/command.h"
#include "command/toolchain.h"
#include "compile/compilation.h"
#include "driver/driver.h"
#include "feature/feature.h"
#include "support/filesystem.h"
#include "syntax/annotation.h"

#include "kota/codec/json/json.h"
#include "llvm/Support/SHA256.h"

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
        help = "Feature to run (folding_range, semantic_tokens) and a source file or directory",
        required = false)
    <std::vector<std::string>> inputs;

    DecoKVStyled(kota::deco::decl::KVStyle::JoinedOrSeparate,
                 names = {"--log-level", "--log-level="},
                 help = "Log level: trace, debug, info, warn, error, off",
                 required = false)
    <std::string> log_level;
};

constexpr std::array feature_names = {"folding_range", "semantic_tokens"};

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

    /// The raw feature payload, absent when compilation failed.
    std::optional<kota::codec::RawValue> result;

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
        llvm::StringRef parent = path::parent_path(dir);
        if(parent == dir) {
            break;
        }
        dir.assign(parent);
    }
    return std::nullopt;
}

std::vector<std::string> error_messages(CompilationUnit& unit) {
    return std::ranges::to<std::vector>(unit.diagnostics() |
                                        std::views::transform(&Diagnostic::message));
}

/// Compile `file` in one pass, deliberately without the preamble PCH the
/// server uses: a shared snapshot pins that the PCH split does not change
/// feature results, so any divergence between the two paths surfaces as a
/// snapshot mismatch instead of hiding in the preamble. Nothing is written
/// to disk.
FileEntry process_file(llvm::StringRef file,
                       llvm::StringRef feature,
                       CompilationDatabase& database,
                       Toolchain& toolchain) {
    FileEntry entry;

    auto buffer = llvm::MemoryBuffer::getFile(file);
    if(!buffer) {
        entry.error = "read_error";
        entry.diagnostics = {buffer.getError().message()};
        return entry;
    }

    auto source = AnnotatedSource::from((*buffer)->getBuffer());
    entry.stripped_hash = sha256_hex(source.content);

    auto commands = database.lookup(file);
    if(commands.empty()) {
        entry.error = "no_compile_command";
        return entry;
    }
    auto& command = commands.front();
    toolchain.resolve_or_warn(command);

    CompilationParams params;
    params.arguments = command.to_argv();
    params.directory = command.resolved.directory.str();
    params.kind = CompilationKind::Content;

    params.add_remapped_file(file, source.content);

    auto unit = clice::compile(params);
    if(!unit.completed()) {
        entry.error = "compile_error";
        entry.diagnostics = error_messages(unit);
        return entry;
    }

    if(feature == "folding_range") {
        entry.result = to_raw_json(feature::folding_ranges(unit));
    } else if(feature == "semantic_tokens") {
        entry.result = to_raw_json(feature::semantic_tokens(unit));
    }
    if(!entry.result.has_value()) {
        entry.error = "serialize_error";
    }
    return entry;
}

bool is_source_file(llvm::StringRef file) {
    auto ext = path::extension(file);
    return ext == ".cpp" || ext == ".cc" || ext == ".cxx";
}

int run_inspect(const InspectOptions& opts) {
    auto& inputs = *opts.inputs;
    llvm::StringRef feature = inputs[0];
    if(!llvm::is_contained(feature_names, feature)) {
        LOG_ERROR("unknown feature '{}', valid: {}", feature, feature_names);
        return 1;
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
            if(!is_source_file(it->path())) {
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

    CompilationDatabase database;
    llvm::StringRef cdb_root = is_dir ? llvm::StringRef(abs_path) : path::parent_path(abs_path);
    if(auto cdb = find_cdb(cdb_root)) {
        if(!database.load(*cdb)) {
            LOG_ERROR("failed to load {}", *cdb);
            return 1;
        }
    } else {
        LOG_WARN("no compile_commands.json above {}; using default flags", cdb_root);
        for(auto& [rel, abs]: files) {
            database.add_command(path::parent_path(abs),
                                 abs,
                                 std::format("clang++ -std=c++20 -fsyntax-only {}", abs));
        }
    }

    Toolchain toolchain;
    InspectOutput output;
    output.feature = feature.str();
    for(auto& [rel, abs]: files) {
        output.files.emplace(rel, process_file(abs, feature, database, toolchain));
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
