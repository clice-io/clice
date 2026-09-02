#include "worker/stateless.h"

#include <atomic>
#include <cstdlib>
#include <expected>
#include <format>
#include <optional>

#include "compile/compilation.h"
#include "feature/feature.h"
#include "index/tu_index.h"
#include "support/logging.h"
#include "support/stderr_sink.h"
#include "worker/common.h"
#include "worker/protocol.h"

#include "kota/async/async.h"
#include "kota/ipc/codec/bincode.h"
#include "kota/ipc/peer.h"
#include "kota/ipc/transport.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"

namespace clice {

/// RAII guard that lowers the current process's scheduling priority and
/// restores it on destruction.
struct ScopedNice {
    int saved;

    explicit ScopedNice(int increment = 10) {
        auto p = kota::sys::priority();
        saved = p ? *p : 0;
        kota::sys::set_priority(saved + increment);
    }

    ~ScopedNice() {
        kota::sys::set_priority(saved);
    }
};

using kota::ipc::RequestResult;
using RequestContext = kota::ipc::BincodePeer::RequestContext;

/// Serialize the preamble's index envelope (full index + document links
/// + inactive regions) into a string. Runs while the freshly parsed AST
/// is still in memory — the only moment the preamble's index is
/// obtainable without deserializing the whole PCH. The file write
/// happens separately, after the PCH itself is flushed.
static std::string serialize_preamble_envelope(CompilationUnit& unit,
                                               std::uint32_t preamble_bound) {
    ScopedTimer links_timer;
    auto links = feature::document_links(unit);
    auto inactive = feature::inactive_regions(unit, {}, 0, preamble_bound);
    auto links_ms = links_timer.ms_f();

    ScopedTimer blob_timer;
    auto blob = index::build_preamble_index(unit, links, inactive.regions, inactive.open_stack);
    LOG_PERF("index_detail",
             "op=preamble links_ms={:.2f} blob_ms={:.2f} bytes={}",
             links_ms,
             blob_timer.ms_f(),
             blob.size());
    return blob;
}

/// Write the serialized blob next to the PCH. Returns an error description
/// on failure so the master's anomaly carries the cause.
static std::optional<std::string> write_preamble_envelope(llvm::StringRef blob,
                                                          llvm::StringRef output_path) {
    std::error_code ec;
    llvm::raw_fd_ostream os(output_path, ec);
    if(ec) {
        auto message =
            std::format("cannot open pch.idx envelope {}: {}", output_path, ec.message());
        LOG_ERROR("BuildPCH: {}", message);
        return message;
    }
    os << blob;
    os.flush();
    if(os.has_error()) {
        auto message = std::format("failed writing pch.idx envelope {}: {}",
                                   output_path,
                                   os.error().message());
        os.clear_error();
        LOG_ERROR("BuildPCH: {}", message);
        return message;
    }
    return std::nullopt;
}

/// Where an artifact build writes: the master's tmp path when it provided
/// one (already allocated by its CacheStore — the master commits with
/// fsync + atomic rename after we report success), else a temporary file
/// of our own.
static std::expected<std::string, std::string> artifact_output(llvm::StringRef label,
                                                               llvm::StringRef output_path,
                                                               llvm::StringRef prefix,
                                                               llvm::StringRef extension) {
    if(!output_path.empty()) {
        return output_path.str();
    }
    auto tmp = fs::createTemporaryFile(prefix, extension);
    if(!tmp) {
        LOG_ERROR("Build{}: failed to create temp file", label);
        return std::unexpected(std::format("Failed to create temporary {} file", label));
    }
    return *tmp;
}

/// The reply of a finished artifact build. Success hands the master the
/// path to commit and the build's inputs; failure removes the half-written
/// file and classifies the errors — `internal_error` marks a failure of
/// the worker's own I/O, never the user's code, and must not be downgraded
/// to an expected build failure.
static worker::ArtifactBuildResult land_artifact(llvm::StringRef label,
                                                 bool success,
                                                 const std::string& tmp_path,
                                                 std::int64_t build_at,
                                                 const auto& deps,
                                                 std::string errors,
                                                 bool internal_error) {
    worker::ArtifactBuildResult result;
    if(success) {
        result.success = true;
        result.output_path = tmp_path;
        result.build_at = build_at;
        result.deps = deps;
        return result;
    }
    fs::remove(tmp_path);
    result.success = false;
    result.has_user_errors = !internal_error && !errors.empty();
    result.error = errors.empty() ? std::format("{} compilation failed", label) : std::move(errors);
    return result;
}

static worker::ArtifactBuildResult handle_build_pch(const worker::BuildPCHParams& params,
                                                    const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    CompilationParams cp;
    cp.kind = CompilationKind::Preamble;
    fill_args(cp, params.directory, params.arguments);
    cp.add_remapped_file(params.file, params.content, params.preamble_bound);
    cp.stop = stop;

    auto output = artifact_output("PCH", params.output_path, "clice-pch", "pch");
    if(!output) {
        return {false, output.error()};
    }
    cp.output_file = *output;

    PCHInfo pch_info;
    ScopedTimer compile_timer;
    auto unit = compile(cp, pch_info);
    auto compile_ms = compile_timer.ms();
    // A cancelled parse reports !completed(); the extra check catches a
    // cancellation landing between the parse and the serialization, whose
    // blob nobody will read. The tmp file is removed like any failed build.
    bool success = unit.completed() && !stop->load(std::memory_order_relaxed);
    auto build_at = unit.build_at().count();

    std::string errors;
    if(!success)
        errors = collect_errors(unit);

    std::string blob;
    ScopedTimer index_timer;
    if(success) {
        blob = serialize_preamble_envelope(unit, params.preamble_bound);
    }
    auto index_ms = index_timer.ms();

    // Destroy CompilationUnit to flush PCH to disk.
    ScopedTimer flush_timer;
    unit = CompilationUnit(nullptr);
    auto flush_ms = flush_timer.ms();

    // Write the blob strictly after the PCH flush: the CacheStore's
    // restart adoption validates a pair by "aux not older than primary"
    // (renames preserve mtimes), so the on-disk order must match the
    // logical one. The PCH is only served together with its blob, so a
    // blob write failure fails the whole build.
    bool internal_error = false;
    ScopedTimer state_write_timer;
    if(success) {
        if(auto error = write_preamble_envelope(blob, params.index_output_path)) {
            success = false;
            internal_error = true;
            errors = std::move(*error);
        }
    }
    auto state_write_ms = state_write_timer.ms();

    if(success) {
        LOG_PERF("build",
                 "kind=pch file={} output={} compile_ms={} preamble_index_ms={} flush_ms={} "
                 "state_write_ms={} total_ms={}",
                 params.file,
                 *output,
                 compile_ms,
                 index_ms,
                 flush_ms,
                 state_write_ms,
                 timer.ms());
    } else {
        LOG_WARN("BuildPCH failed: file={}, {}ms, errors=[{}]", params.file, timer.ms(), errors);
    }
    return land_artifact("PCH",
                         success,
                         *output,
                         build_at,
                         pch_info.deps,
                         std::move(errors),
                         internal_error);
}

static worker::ArtifactBuildResult handle_build_pcm(const worker::BuildPCMParams& params,
                                                    const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    CompilationParams cp;
    cp.kind = CompilationKind::ModuleInterface;
    fill_args(cp, params.directory, params.arguments);
    for(auto& [name, path]: params.pcms) {
        cp.pcms.try_emplace(name, path);
    }
    cp.stop = stop;

    auto output = artifact_output("PCM", params.output_path, "clice-pcm", "pcm");
    if(!output) {
        return {false, output.error()};
    }
    cp.output_file = *output;

    PCMInfo pcm_info;
    ScopedTimer compile_timer;
    auto unit = compile(cp, pcm_info);
    auto compile_ms = compile_timer.ms();
    bool success = unit.completed() && !stop->load(std::memory_order_relaxed);
    auto build_at = unit.build_at().count();

    std::string errors;
    if(!success)
        errors = collect_errors(unit);

    // TODO: PCM indexing. Unlike the PCH, a PCM is not a transient
    // buffer-derived artifact — module units are ordinary disk files with
    // CDB entries, so their symbols should flow through the normal
    // background-indexing path (no per-blob pair needed).
    ScopedTimer flush_timer;
    unit = CompilationUnit(nullptr);
    auto flush_ms = flush_timer.ms();

    if(success) {
        LOG_PERF("build",
                 "kind=pcm module={} compile_ms={} flush_ms={} total_ms={}",
                 params.module_name,
                 compile_ms,
                 flush_ms,
                 timer.ms());
    } else {
        LOG_WARN("BuildPCM failed: module={}, {}ms, errors=[{}]",
                 params.module_name,
                 timer.ms(),
                 errors);
    }
    return land_artifact("PCM",
                         success,
                         *output,
                         build_at,
                         pcm_info.deps,
                         std::move(errors),
                         /*internal_error=*/false);
}

/// Collect the tidy pass's findings with real per-file locations: unlike
/// the LSP path, which folds header diagnostics onto their include line,
/// the CLI reports them where they are. clang-tidy's header-filter
/// contract is applied here — our diagnostic path has no
/// ClangTidyDiagnosticConsumer to apply it: main-file findings always
/// report; a header finding needs HeaderFilterRegex to match (empty =
/// main file only) and must not match ExcludeHeaderFilterRegex; system
/// headers report only under SystemHeaders. Compiler errors are kept
/// regardless of location, as clang-tidy keeps them: a parse can complete
/// with a usable AST despite errors, and a run that discarded them would
/// pass broken code.
static void collect_tidy_diagnostics(CompilationUnitRef unit,
                                     const worker::TURunParams& params,
                                     std::vector<worker::TidyDiagnostic>& out) {
    auto main_fid = unit.main_file();
    std::optional<llvm::Regex> keep;
    if(!params.tidy_header_filter.empty()) {
        keep.emplace(params.tidy_header_filter);
    }
    std::optional<llvm::Regex> drop;
    if(!params.tidy_exclude_header_filter.empty()) {
        drop.emplace(params.tidy_exclude_header_filter);
    }

    for(const auto& raw: unit.diagnostics()) {
        bool clang_error =
            raw.id.source == DiagnosticSource::Clang &&
            (raw.id.level == DiagnosticLevel::Error || raw.id.level == DiagnosticLevel::Fatal);
        if(!clang_error && (raw.id.source != DiagnosticSource::ClangTidy ||
                            raw.id.level == DiagnosticLevel::Ignored)) {
            continue;
        }
        if(raw.fid.isInvalid() || !raw.range.valid()) {
            continue;
        }
        auto file = unit.file_path(raw.fid);
        if(raw.fid != main_fid && !clang_error) {
            if(raw.in_system && !params.tidy_system_headers) {
                continue;
            }
            if(!keep || !keep->match(file)) {
                continue;
            }
            if(drop && drop->match(file)) {
                continue;
            }
        }
        feature::LineMap map(unit.file_content(raw.fid), feature::PositionEncoding::UTF8);
        auto range = feature::to_range(map, raw.range);
        if(!range) {
            continue;
        }
        out.push_back({
            .file = std::string(file),
            .line = range->start.line + 1,
            .column = range->start.character + 1,
            .error =
                raw.id.level == DiagnosticLevel::Error || raw.id.level == DiagnosticLevel::Fatal,
            .message = raw.message,
            // clang-tidy's name for compiler errors; plain errors carry no
            // warning-option name of their own.
            .check = clang_error ? "clang-diagnostic-error" : std::string(raw.id.name),
        });
    }
}

static worker::TURunResult handle_turun(const worker::TURunParams& params,
                                        const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    CompilationParams cp;
    // One parse serves every product of the plan. Tidy's matcher walks the
    // collected top-level declarations, which only a Content build
    // gathers; a pure index run keeps the Indexing kind.
    cp.kind = params.tidy ? CompilationKind::Content : CompilationKind::Indexing;
    fill_args(cp, params.directory, params.arguments);
    for(auto& [name, path]: params.pcms) {
        cp.pcms.try_emplace(name, path);
    }
    if(params.tidy) {
        // The command-affecting extra args are already in params.arguments
        // (applied at driver resolution); the copies here feed the
        // engine's warning-options path only.
        cp.tidy = tidy::TidyParams{.checks = params.tidy_checks,
                                   .fast_only = false,
                                   .options = params.tidy_options,
                                   .warnings_as_errors = params.tidy_warnings_as_errors,
                                   .header_filter = params.tidy_header_filter,
                                   .exclude_header_filter = params.tidy_exclude_header_filter,
                                   .system_headers = params.tidy_system_headers,
                                   .extra_args = params.tidy_extra_args,
                                   .extra_args_before = params.tidy_extra_args_before};
    }
    cp.stop = stop;

    ScopedTimer compile_timer;
    auto unit = compile(cp);
    auto compile_ms = compile_timer.ms();
    if(!unit.completed()) {
        LOG_WARN("TU run failed: file={}, {}ms", params.file, timer.ms());
        return {false, "TU run compilation failed"};
    }

    // Building and serializing the index costs a large share of the pass;
    // skip it when the cancellation landed after the parse finished.
    if(stop->load(std::memory_order_relaxed)) {
        return {false, "TU run cancelled"};
    }
    worker::TURunResult result;
    result.success = true;
    ScopedTimer index_timer;
    if(params.index) {
        result.tu_index_data = index::build_tu_index(unit);
    }
    auto index_ms = index_timer.ms();
    if(params.tidy) {
        collect_tidy_diagnostics(unit, params, result.tidy_diagnostics);
    }

    // AST teardown for a large TU is material work that belongs to this
    // task: sample the total only after the unit is gone, so the logged
    // span covers everything that blocks the worker.
    ScopedTimer teardown_timer;
    unit = CompilationUnit(nullptr);
    auto teardown_ms = teardown_timer.ms();

    LOG_PERF(
        "build",
        "kind=turun file={} bytes={} findings={} compile_ms={} index_ms={} teardown_ms={} total_ms={}",
        params.file,
        result.tu_index_data.size(),
        result.tidy_diagnostics.size(),
        compile_ms,
        index_ms,
        teardown_ms,
        timer.ms());
    return result;
}

static kota::codec::RawValue handle_completion(const worker::CompletionParams& params,
                                               const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    CompilationParams cp;
    cp.kind = CompilationKind::Completion;
    fill_args(cp, params.directory, params.arguments);
    if(!params.pch.first.empty()) {
        cp.pch = params.pch;
    }
    for(auto& [name, path]: params.pcms) {
        cp.pcms.try_emplace(name, path);
    }
    cp.add_remapped_file(params.file, params.text);
    cp.completion = {params.file, params.offset};
    cp.stop = stop;

    auto items = feature::code_complete(cp, params.config.code_completion);
    LOG_DEBUG("Completion done: {} items, {}ms", items.size(), timer.ms());

    return to_raw(items);
}

static kota::codec::RawValue handle_signature_help(const worker::SignatureHelpParams& params,
                                                   const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    CompilationParams cp;
    cp.kind = CompilationKind::Completion;
    fill_args(cp, params.directory, params.arguments);
    if(!params.pch.first.empty()) {
        cp.pch = params.pch;
    }
    for(auto& [name, path]: params.pcms) {
        cp.pcms.try_emplace(name, path);
    }
    cp.add_remapped_file(params.file, params.text);
    cp.completion = {params.file, params.offset};
    cp.stop = stop;

    auto help = feature::signature_help(cp);
    LOG_DEBUG("SignatureHelp done: {}ms", timer.ms());

    return to_raw(help);
}

static kota::codec::RawValue handle_format(const worker::FormatParams& params) {
    ScopedTimer timer;

    std::optional<LocalSourceRange> range;
    if(params.range.valid()) {
        range = params.range;
    }

    auto edits = feature::document_format(params.file, params.text, range);
    LOG_DEBUG("Format done: {} edits, {}ms", edits.size(), timer.ms());

    return to_raw(edits);
}

/// Register the handler of one request type. Each request arms a fresh
/// stop flag as the most recent build's — published before the
/// pool-thread hop so a CancelBuild aimed at it still lands — and runs
/// the handler on the pool thread. A cancellation (peer close, wire-level
/// $/cancelRequest) dequeues work that has not started, which answers
/// `cancelled`; work already on the pool thread learns through the hook:
/// the flag doubles as CompilationParams::stop, which clang polls after
/// every top-level declaration, so even the parse itself stops instead of
/// running to completion for a result nobody will read.
template <typename Params, typename Result, typename Handler>
static void serve(kota::ipc::BincodePeer& peer,
                  std::shared_ptr<std::atomic_bool>& build_stop,
                  Result cancelled,
                  Handler handler) {
    peer.on_request([&build_stop,
                     cancelled,
                     handler](RequestContext&, const Params& params) -> RequestResult<Params> {
        auto stop = std::make_shared<std::atomic_bool>(false);
        build_stop = stop;
        auto result = co_await kota::queue(
            [&]() -> Result {
                if(stop->load(std::memory_order_relaxed)) {
                    return cancelled;
                }
                return handler(params, stop);
            },
            [stop] { stop->store(true, std::memory_order_relaxed); });
        co_return result.value();
    });
}

int run_stateless_worker_mode(const std::string& worker_name, const std::string& log_dir) {
    // Limit libuv thread pool to 1 thread so each stateless worker executes
    // only one compilation at a time. Must be set before any kota::queue call.
    // FIXME: return values of setenv/_putenv_s are unchecked; a failure would
    // silently fall back to libuv's default pool size.
#ifdef _WIN32
    _putenv_s("UV_THREADPOOL_SIZE", "1");
#else
    ::setenv("UV_THREADPOOL_SIZE", "1", 1);
#endif

    logging::stderr_logger(worker_name, logging::options);
    // A worker's stderr reader is the master's always-running drain — a
    // trusted party — and the fd is reserved for third-party crash output
    // (assertion failures, sanitizer reports) whose writers expect blocking
    // semantics. Undo the sink's non-blocking switch unconditionally: with
    // no log directory the file_logger below never runs.
    logging::restore_pipe_blocking();
    if(!log_dir.empty()) {
        // File only: worker stderr is reserved for crash/unexpected output,
        // which the master relays into its own log (see logging taxonomy).
        logging::file_logger(worker_name, log_dir, logging::options, /*mirror_stderr=*/false);
    }

    LOG_INFO("Starting stateless worker");

    kota::event_loop loop;

    auto transport_result = kota::ipc::StreamTransport::open_stdio(loop);
    if(!transport_result) {
        LOG_ERROR("Failed to open stdio transport");
        return 1;
    }

    // Stop flag of the most recent build request, published before its
    // pool-thread hop so a CancelBuild aimed at it still lands. Never
    // cleared: the master sends CancelBuild only while it awaits that
    // build's reply, and pipe ordering pins any follow-up build behind the
    // cancel, so a set can only ever hit the stale build's flag.
    std::shared_ptr<std::atomic_bool> build_stop;

    kota::ipc::BincodePeer peer(loop, std::move(*transport_result));

    peer.on_notification([&build_stop](const worker::CancelBuildParams&) {
        LOG_DEBUG("CancelBuild notification received");
        if(build_stop) {
            build_stop->store(true, std::memory_order_relaxed);
        }
    });

    const worker::ArtifactBuildResult cancelled_build{.success = false, .error = "Build cancelled"};
    serve<worker::BuildPCHParams>(peer, build_stop, cancelled_build, &handle_build_pch);
    serve<worker::BuildPCMParams>(peer, build_stop, cancelled_build, &handle_build_pcm);
    serve<worker::TURunParams>(
        peer,
        build_stop,
        worker::TURunResult{.success = false, .error = "Build cancelled"},
        [](const worker::TURunParams& params, const std::shared_ptr<std::atomic_bool>& stop) {
            ScopedNice guard;
            return handle_turun(params, stop);
        });
    const kota::codec::RawValue cancelled_query{"null"};
    serve<worker::CompletionParams>(peer, build_stop, cancelled_query, &handle_completion);
    serve<worker::SignatureHelpParams>(peer, build_stop, cancelled_query, &handle_signature_help);
    serve<worker::FormatParams>(
        peer,
        build_stop,
        cancelled_query,
        [](const worker::FormatParams& params, const std::shared_ptr<std::atomic_bool>&) {
            return handle_format(params);
        });

    LOG_INFO("Stateless worker ready, waiting for requests");
    loop.schedule(peer.run());
    auto ret = loop.run();
    LOG_INFO("Stateless worker exiting with code {}", ret);
    return ret;
}

}  // namespace clice
