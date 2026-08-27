#include "worker/stateless.h"

#include <atomic>
#include <cstdlib>
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

static worker::ArtifactBuildResult handle_build_pch(const worker::BuildPCHParams& params,
                                                    const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    CompilationParams cp;
    cp.kind = CompilationKind::Preamble;
    fill_args(cp, params.directory, params.arguments);
    cp.add_remapped_file(params.file, params.content, params.preamble_bound);
    cp.stop = stop;

    // When the master provides an output path it is already a tmp path
    // allocated by its CacheStore: write directly, the master commits
    // (fsync + atomic rename) after we report success.
    std::string tmp_path;
    if(!params.output_path.empty()) {
        tmp_path = params.output_path;
    } else {
        auto tmp = fs::createTemporaryFile("clice-pch", "pch");
        if(!tmp) {
            LOG_ERROR("BuildPCH: failed to create temp file");
            return {false, "Failed to create temporary PCH file"};
        }
        tmp_path = *tmp;
    }
    cp.output_file = tmp_path;

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
    // blob write failure fails the whole build. It is an internal I/O
    // failure, never a user-code problem — must not be downgraded to an
    // expected build failure.
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
                 tmp_path,
                 compile_ms,
                 index_ms,
                 flush_ms,
                 state_write_ms,
                 timer.ms());
        worker::ArtifactBuildResult result;
        result.success = true;
        result.output_path = tmp_path;
        result.build_at = build_at;
        result.deps = pch_info.deps;
        return result;
    } else {
        LOG_WARN("BuildPCH failed: file={}, {}ms, errors=[{}]", params.file, timer.ms(), errors);
        fs::remove(tmp_path);
        worker::ArtifactBuildResult result;
        result.success = false;
        result.error = errors.empty() ? "PCH compilation failed" : errors;
        result.has_user_errors = !internal_error && !errors.empty();
        return result;
    }
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

    // See handle_build_pch: a provided output path is the master's tmp path.
    std::string tmp_path;
    if(!params.output_path.empty()) {
        tmp_path = params.output_path;
    } else {
        auto tmp = fs::createTemporaryFile("clice-pcm", "pcm");
        if(!tmp) {
            LOG_ERROR("BuildPCM: failed to create temp file");
            return {false, "Failed to create temporary PCM file"};
        }
        tmp_path = *tmp;
    }
    cp.output_file = tmp_path;

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
        worker::ArtifactBuildResult result;
        result.success = true;
        result.output_path = tmp_path;
        result.build_at = build_at;
        result.deps = pcm_info.deps;
        return result;
    } else {
        LOG_WARN("BuildPCM failed: module={}, {}ms, errors=[{}]",
                 params.module_name,
                 timer.ms(),
                 errors);
        fs::remove(tmp_path);
        worker::ArtifactBuildResult result;
        result.success = false;
        result.error = errors.empty() ? "PCM compilation failed" : errors;
        result.has_user_errors = !errors.empty();
        return result;
    }
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
    auto main_fid = unit.interested_file();
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

    // A cancellation (peer close, wire-level $/cancelRequest) dequeues
    // work that has not started; work already on the pool thread learns
    // through the hook: the shared flag doubles as CompilationParams::
    // stop, which clang polls after every top-level declaration, so even
    // the parse itself stops instead of running to completion for a
    // result nobody will read.
    auto arm_stop = [&build_stop] {
        auto stop = std::make_shared<std::atomic_bool>(false);
        build_stop = stop;
        return stop;
    };

    peer.on_request(
        [&](RequestContext& ctx,
            const worker::BuildPCHParams& params) -> RequestResult<worker::BuildPCHParams> {
            auto stop = arm_stop();
            auto result = co_await kota::queue(
                [&]() -> worker::ArtifactBuildResult {
                    if(stop->load(std::memory_order_relaxed)) {
                        return {false, "Build cancelled"};
                    }
                    return handle_build_pch(params, stop);
                },
                [stop] { stop->store(true, std::memory_order_relaxed); });
            co_return result.value();
        });

    peer.on_request(
        [&](RequestContext& ctx,
            const worker::BuildPCMParams& params) -> RequestResult<worker::BuildPCMParams> {
            auto stop = arm_stop();
            auto result = co_await kota::queue(
                [&]() -> worker::ArtifactBuildResult {
                    if(stop->load(std::memory_order_relaxed)) {
                        return {false, "Build cancelled"};
                    }
                    return handle_build_pcm(params, stop);
                },
                [stop] { stop->store(true, std::memory_order_relaxed); });
            co_return result.value();
        });

    peer.on_request([&](RequestContext& ctx,
                        const worker::TURunParams& params) -> RequestResult<worker::TURunParams> {
        auto stop = arm_stop();
        auto result = co_await kota::queue(
            [&]() -> worker::TURunResult {
                if(stop->load(std::memory_order_relaxed)) {
                    return {false, "Build cancelled"};
                }
                ScopedNice guard;
                return handle_turun(params, stop);
            },
            [stop] { stop->store(true, std::memory_order_relaxed); });
        co_return result.value();
    });

    peer.on_request(
        [&](RequestContext& ctx,
            const worker::CompletionParams& params) -> RequestResult<worker::CompletionParams> {
            auto stop = arm_stop();
            auto result = co_await kota::queue(
                [&]() -> kota::codec::RawValue {
                    if(stop->load(std::memory_order_relaxed)) {
                        return kota::codec::RawValue{"null"};
                    }
                    return handle_completion(params, stop);
                },
                [stop] { stop->store(true, std::memory_order_relaxed); });
            co_return result.value();
        });

    peer.on_request([&](RequestContext& ctx, const worker::SignatureHelpParams& params)
                        -> RequestResult<worker::SignatureHelpParams> {
        auto stop = arm_stop();
        auto result = co_await kota::queue(
            [&]() -> kota::codec::RawValue {
                if(stop->load(std::memory_order_relaxed)) {
                    return kota::codec::RawValue{"null"};
                }
                return handle_signature_help(params, stop);
            },
            [stop] { stop->store(true, std::memory_order_relaxed); });
        co_return result.value();
    });

    peer.on_request([&](RequestContext& ctx,
                        const worker::FormatParams& params) -> RequestResult<worker::FormatParams> {
        auto stop = arm_stop();
        auto result = co_await kota::queue(
            [&]() -> kota::codec::RawValue {
                if(stop->load(std::memory_order_relaxed)) {
                    return kota::codec::RawValue{"null"};
                }
                return handle_format(params);
            },
            [stop] { stop->store(true, std::memory_order_relaxed); });
        co_return result.value();
    });

    LOG_INFO("Stateless worker ready, waiting for requests");
    loop.schedule(peer.run());
    auto ret = loop.run();
    LOG_INFO("Stateless worker exiting with code {}", ret);
    return ret;
}

}  // namespace clice
