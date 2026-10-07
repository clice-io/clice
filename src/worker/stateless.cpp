module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

#include "kota/ipc/framing.h"

module clice;

import :compile.compilation;
import :feature.feature;
import :index.tu_index;
import :support.logging;
import :support.process;
import :vfs.file_system;
import :worker.common;
import :worker.crash_report;
import :worker.protocol;
import :worker.stateless;

namespace clice {

using kota::ipc::RequestResult;
using RequestContext = kota::ipc::BincodePeer::RequestContext;

/// Serialize the preamble's index envelope (full index + document links
/// + inactive regions + diagnostics) into a string. Runs while the freshly parsed AST
/// is still in memory — the only moment the preamble's index is
/// obtainable without deserializing the whole PCH. The file write
/// happens separately, after the PCH itself is flushed.
static std::string serialize_preamble_envelope(CompilationUnit& unit,
                                               std::uint32_t preamble_bound) {
    ScopedTimer links_timer;
    auto links = feature::document_links(unit);
    auto inactive = feature::inactive_regions(unit, {}, 0, preamble_bound);
    auto links_ms = links_timer.ms_f();
    auto diagnostics = to_client_json(feature::diagnostics(unit), "[]");

    ScopedTimer blob_timer;
    auto blob = index::build_preamble_index(unit,
                                            links,
                                            inactive.regions,
                                            inactive.open_stack,
                                            diagnostics);
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
    if(auto error = vfs::write(output_path, blob)) {
        auto message =
            std::format("failed writing pch.idx envelope {}: {}", output_path, error.message());
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
    auto tmp = vfs::temp_file(prefix, extension);
    if(!tmp) {
        LOG_ERROR("Build{}: failed to create temp file", label);
        return std::unexpected(std::format("Failed to create temporary {} file", label));
    }
    return *tmp;
}

/// The reply of a finished artifact build. Success hands the master the
/// path to commit; failure removes the half-written file and classifies
/// the errors — `internal_error` marks a failure of the worker's own I/O,
/// never the user's code, and must not be downgraded to an expected build
/// failure. Either way the reply carries the build's inputs.
static worker::ArtifactBuildResult land_artifact(llvm::StringRef label,
                                                 bool success,
                                                 const std::string& tmp_path,
                                                 std::int64_t build_at,
                                                 const auto& deps,
                                                 std::string errors,
                                                 bool internal_error) {
    worker::ArtifactBuildResult result;
    result.build_at = build_at;
    result.deps = deps;
    if(success) {
        result.success = true;
        result.output_path = tmp_path;
        return result;
    }
    vfs::remove(tmp_path);
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
    cp.workspace = params.workspace;
    cp.add_remapped_file(params.file, params.content, params.preamble_bound);
    cp.add_synthesized(params.synthesized);
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
    use_artifacts(cp, {}, params.pcms);
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

/// Where a diagnostic starts, its column counted in bytes as compilers
/// count them.
static auto byte_position(CompilationUnitRef unit, const Diagnostic& diagnostic)
    -> std::optional<kota::ipc::protocol::Position> {
    auto content = unit.file_content(diagnostic.fid);
    return kota::ipc::lsp::to_position({content.data(), content.size()},
                                       diagnostic.range.begin,
                                       kota::ipc::lsp::PositionEncoding::UTF8);
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
/// pass broken code. Notes follow their finding in the stream and attach
/// to it.
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

    bool last_kept = false;
    for(const auto& raw: unit.diagnostics()) {
        if(raw.id.level == DiagnosticLevel::Note) {
            if(!last_kept || raw.fid.isInvalid() || !raw.range.valid()) {
                continue;
            }
            if(auto start = byte_position(unit, raw)) {
                out.back().notes.push_back({
                    .file = std::string(unit.file_path(raw.fid)),
                    .line = start->line + 1,
                    .column = start->character + 1,
                    .message = raw.message,
                });
            }
            continue;
        }
        last_kept = false;
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
            // Matched like clang-tidy does: against the name the lookup
            // reached the header by, not its identity; a buffer no file
            // backs is kept.
            auto entry = unit.context().getSourceManager().getFileEntryRefForID(raw.fid);
            if(entry && (!keep || !keep->match(entry->getName()) ||
                         (drop && drop->match(entry->getName())))) {
                continue;
            }
        }
        auto start = byte_position(unit, raw);
        if(!start) {
            continue;
        }
        out.push_back({
            .file = std::string(file),
            .line = start->line + 1,
            .column = start->character + 1,
            .error =
                raw.id.level == DiagnosticLevel::Error || raw.id.level == DiagnosticLevel::Fatal,
            .message = raw.message,
            // clang-tidy's name for compiler errors; plain errors carry no
            // warning-option name of their own.
            .check = clang_error ? "clang-diagnostic-error" : std::string(raw.id.name),
        });
        last_kept = true;
    }
}

static worker::TURunResult handle_turun(const worker::TURunParams& params,
                                        const std::shared_ptr<std::atomic_bool>& stop) {
    LOG_INFO("TURun request: file={}", params.file);
    ScopedTimer timer;

    CompilationParams cp;
    // One parse serves every product of the plan: the tidy pass runs on a
    // Content build, the plain AST build; a pure index run keeps the
    // Indexing kind.
    cp.kind = params.tidy ? CompilationKind::Content : CompilationKind::Indexing;
    fill_args(cp, params.directory, params.arguments);
    cp.workspace = params.workspace;
    cp.add_synthesized(params.synthesized);
    use_artifacts(cp, {}, params.pcms);
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
                                   .extra_args_before = params.tidy_extra_args_before,
                                   .whole_tu = true};
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
    std::size_t index_bytes = 0;
    if(params.index) {
        auto envelope = index::build_tu_index(unit, {.known_variants = params.known_variants});
        index_bytes = envelope.size();
        if(auto error = hand_over_index(std::move(envelope), params.index_output_path, result)) {
            return {false, std::move(*error)};
        }
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
        index_bytes,
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
    use_artifacts(cp, params.pch, params.pcms);
    cp.add_remapped_file(params.file, params.text);
    cp.add_synthesized(params.synthesized);
    cp.completion = {params.file, params.offset};
    cp.stop = stop;

    auto items = feature::code_complete(cp, params.config.code_completion, params.client);
    LOG_DEBUG("Completion done: {} items, {}ms", items.size(), timer.ms());

    return to_raw(items);
}

static kota::codec::RawValue handle_signature_help(const worker::SignatureHelpParams& params,
                                                   const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    CompilationParams cp;
    cp.kind = CompilationKind::Completion;
    fill_args(cp, params.directory, params.arguments);
    use_artifacts(cp, params.pch, params.pcms);
    cp.add_remapped_file(params.file, params.text);
    cp.add_synthesized(params.synthesized);
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

/// How serve() runs a request's work.
struct ServeOptions {
    /// Background work: run it on a thread of its own at lowered priority,
    /// which ends with the request. Lowering the pool thread instead would
    /// leave every later request on it lowered (see lower_thread_priority).
    bool lowered = false;
};

/// Run `work` on a fresh thread of lowered priority and wait for it.
template <typename Work>
static auto run_lowered(Work& work) {
    std::optional<std::invoke_result_t<Work&>> result;
    // As much stack as libuv gives its pool threads; a deep parse needs it.
    llvm::thread thread(std::optional<unsigned>(8u << 20), [&] {
        lower_thread_priority();
        result.emplace(work());
    });
    thread.join();
    return std::move(*result);
}

/// Register the handler of one request type, which runs on the pool
/// thread, or on a lowered thread it joins (ServeOptions::lowered). A
/// cancellation (peer close, $/cancelRequest) dequeues work that
/// has not started; work already on the pool thread learns through the
/// hook's stop flag, which doubles as CompilationParams::stop: clang polls
/// it after every top-level declaration, so even the parse itself stops
/// instead of running to completion for a result nobody will read.
template <typename Params, typename Result, typename Handler>
static void serve(kota::ipc::BincodePeer& peer,
                  Result cancelled,
                  Handler handler,
                  ServeOptions options = {}) {
    peer.on_request([cancelled, handler, options](RequestContext&,
                                                  const Params& params) -> RequestResult<Params> {
        auto stop = std::make_shared<std::atomic_bool>(false);
        co_return co_await kota::queue(
            [&]() -> Result {
                if(stop->load(std::memory_order_relaxed)) {
                    return cancelled;
                }
                // The crash report reads the scope of the thread that
                // faults, so it opens on the thread that runs the work.
                auto work = [&] {
                    CrashScope crash_scope(worker::crash_tag(params));
                    return handler(params, stop);
                };
                auto result = options.lowered ? run_lowered(work) : work();
                release_free_memory();
                return result;
            },
            [stop] { stop->store(true, std::memory_order_relaxed); });
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
    if(!log_dir.empty()) {
        // File only: worker stderr is reserved for crash/unexpected output,
        // which the master relays into its own log (see logging taxonomy).
        logging::file_logger(worker_name, log_dir, logging::options, /*mirror_stderr=*/false);
    }

    LOG_INFO("Starting stateless worker");
    install_crash_report();

    kota::event_loop loop;

    auto transport_result = kota::ipc::StreamTransport::open_stdio(loop);
    if(!transport_result) {
        LOG_ERROR("Failed to open stdio transport");
        return 1;
    }
    (*transport_result)->set_remote_max_payload(kota::ipc::default_max_payload);

    kota::ipc::BincodePeer peer(loop, std::move(*transport_result));

    const worker::ArtifactBuildResult cancelled_build{.success = false, .error = "Build cancelled"};
    serve<worker::BuildPCHParams>(peer, cancelled_build, &handle_build_pch);
    serve<worker::BuildPCMParams>(peer, cancelled_build, &handle_build_pcm);
    serve<worker::TURunParams>(peer,
                               worker::TURunResult{.success = false, .error = "Build cancelled"},
                               &handle_turun,
                               {.lowered = true});
    const kota::codec::RawValue cancelled_query{"null"};
    serve<worker::CompletionParams>(peer, cancelled_query, &handle_completion);
    serve<worker::SignatureHelpParams>(peer, cancelled_query, &handle_signature_help);
    serve<worker::FormatParams>(
        peer,
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
