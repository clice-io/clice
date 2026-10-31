module;

#include "kotatsu/ipc/framing.h"

module clice;

import :compile.compilation;
import :compile.diagnostic;
import :feature.code_completion;
import :feature.diagnostics;
import :feature.document_links;
import :feature.formatting;
import :feature.inactive_regions;
import :feature.signature_help;
import :index.include_tree;
import :index.indexer;
import :index.tu_index;
import :support.bytes;
import :support.json;
import :support.logging;
import :support.process;
import :syntax.position;
import :vfs.file_system;
import :worker.common;
import :worker.crash_report;
import :worker.gate;
import :worker.protocol;
import :worker.stateless;

namespace clice {

using ipc::RequestResult;
using RequestContext = ipc::BincodePeer::RequestContext;

/// Serialize the preamble's index blob (full index + document links
/// + inactive regions + diagnostics) into a string. Runs while the freshly parsed AST
/// is still in memory — the only moment the preamble's index is
/// obtainable without deserializing the whole PCH. The file write
/// happens separately, after the PCH itself is flushed.
static std::vector<std::byte> serialize_preamble_index(CompilationUnit& unit,
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
static std::optional<std::string> write_preamble_index(llvm::StringRef blob,
                                                       llvm::StringRef output_path) {
    if(auto error = fs::write_file(output_path, blob)) {
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
    std::string dir;
    sys::temp_dir(dir);
    std::string path;
    if(!fs::create_temp_file(path, dir, prefix.str() + "-", "." + extension.str())) {
        LOG_ERROR("Build{}: failed to create temp file", label);
        return std::unexpected(std::format("Failed to create temporary {} file", label));
    }
    return path;
}

/// The reply of a finished artifact build. Success hands the master the
/// path to commit; failure removes the half-written file and classifies
/// the errors — `internal_error` marks a failure of the worker's own I/O,
/// never the user's code, and must not be downgraded to an expected build
/// failure. Either way the reply carries the build's inputs.
static worker::ArtifactBuildResult finish_artifact(llvm::StringRef label,
                                                   bool success,
                                                   const std::string& tmp_path,
                                                   std::int64_t build_at,
                                                   const auto& deps,
                                                   llvm::ArrayRef<std::string> errors,
                                                   bool internal_error) {
    worker::ArtifactBuildResult result;
    result.build_at = build_at;
    result.deps = deps;
    if(success) {
        result.success = true;
        result.output_path = tmp_path;
        return result;
    }
    fs::remove(tmp_path);
    result.success = false;
    // FIXME: clang failing to open its output (the cache directory turned
    // unwritable or full mid-session) counts as the user's error: the master keeps
    // the failure on unchanged inputs until a consumer is saved.
    result.has_user_errors = !internal_error && !errors.empty();
    result.error =
        errors.empty() ? std::format("{} compilation failed", label) : llvm::join(errors, "; ");
    if(result.has_user_errors) {
        result.first_error = errors.front();
    }
    return result;
}

/// What an artifact build read; a build that failed on errors read just as
/// much. The source of a PCM, or of a whole file's PCH, is an input of the
/// build all the same, and unlike a preamble's PCH key their cache keys
/// embed no content: only the deps see the source change. Named by its
/// identity like every other dep: the command spells it as written, which
/// consumers cannot stat reliably.
static std::vector<DepFile> artifact_deps(CompilationUnit& unit, bool source) {
    std::vector<DepFile> deps;
    if(!unit.completed()) {
        return deps;
    }
    deps = unit.deps();
    if(source) {
        auto focus = unit.focus_file();
        deps.emplace_back(std::string(unit.file_path(focus.file())),
                          llvm::xxh3_64bits(focus.content()));
    }
    return deps;
}

static worker::ArtifactBuildResult handle_build_pch(const worker::BuildPCHParams& params,
                                                    const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    auto output = artifact_output("PCH", params.output_path, "clice-pch", "pch");
    if(!output) {
        return {false, output.error()};
    }

    CompilationParams cp{
        .kind = params.whole_file ? CompilationKind::PrecompiledHeader : CompilationKind::Preamble,
        .input = {.arguments = params.arguments,
                  .directory = params.directory,
                  .project_root = params.project_root,
                  .vfs = llvm::makeIntrusiveRefCnt<vfs::View>(params.hidden)},
        .output_file = *output,
        .skip_bodies = params.skip_bodies,
        .stop = stop,
    };
    if(!params.whole_file) {
        cp.input.add_remapped_file(params.file, params.content, params.preamble_bound);
    }
    cp.input.add_synthesized(params.synthesized);
    use_artifacts(cp.input, {params.pch, 0}, params.pcms);

    ScopedTimer compile_timer;
    auto unit = compile(std::move(cp));
    auto compile_ms = compile_timer.ms();
    // A cancelled parse has no output; the stop check catches a
    // cancellation arriving between the parse and the serialization, whose
    // blob nobody will read. The tmp file is removed like any failed build.
    bool success = unit->has_output() && !stop->load(std::memory_order_relaxed);
    auto build_at = unit->build_at().count();
    auto deps = artifact_deps(*unit, params.whole_file);

    std::vector<std::string> errors;
    if(!success)
        errors = collect_errors(*unit);

    std::vector<std::byte> blob;
    ScopedTimer index_timer;
    if(success) {
        blob = serialize_preamble_index(*unit, params.whole_file ? 0 : params.preamble_bound);
    }
    auto index_ms = index_timer.ms();

    // Destroying the unit writes the PCH to disk.
    ScopedTimer flush_timer;
    unit.reset();
    auto flush_ms = flush_timer.ms();

    // Write the blob strictly after the PCH flush: the CacheStore's
    // restart adoption validates a pair by "aux not older than primary"
    // (renames preserve mtimes), so the on-disk order must match the
    // logical one. The PCH is only served together with its blob, so a
    // blob write failure fails the whole build.
    bool internal_error = false;
    ScopedTimer state_write_timer;
    if(success) {
        if(auto error = write_preamble_index(as_chars(blob), params.index_output_path)) {
            success = false;
            internal_error = true;
            errors = {std::move(*error)};
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
        LOG_WARN("BuildPCH failed: file={}, {}ms, errors=[{}]",
                 params.file,
                 timer.ms(),
                 llvm::join(errors, "; "));
    }
    return finish_artifact("PCH", success, *output, build_at, deps, errors, internal_error);
}

static worker::ArtifactBuildResult handle_build_pcm(const worker::BuildPCMParams& params,
                                                    const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    auto output = artifact_output("PCM", params.output_path, "clice-pcm", "pcm");
    if(!output) {
        return {false, output.error()};
    }

    CompilationParams cp{
        .kind = CompilationKind::ModuleInterface,
        .input = {.arguments = params.arguments,
                  .directory = params.directory,
                  .vfs = llvm::makeIntrusiveRefCnt<vfs::View>(params.hidden)},
        .output_file = *output,
        .stop = stop,
    };
    use_artifacts(cp.input, {params.pch, 0}, params.pcms);

    ScopedTimer compile_timer;
    auto unit = compile(std::move(cp));
    auto compile_ms = compile_timer.ms();
    bool success = unit->has_output() && !stop->load(std::memory_order_relaxed);
    auto build_at = unit->build_at().count();
    auto deps = artifact_deps(*unit, true);

    std::vector<std::string> errors;
    if(!success)
        errors = collect_errors(*unit);

    // No index for the PCM: module units are ordinary units of the database,
    // indexed by the background index like any other.
    ScopedTimer flush_timer;
    unit.reset();
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
                 llvm::join(errors, "; "));
    }
    return finish_artifact("PCM",
                           success,
                           *output,
                           build_at,
                           deps,
                           errors,
                           /*internal_error=*/false);
}

/// The batch tidy pass's findings, each where it is: unlike the LSP path,
/// which folds header diagnostics onto their include line, the CLI reports
/// them in their own files. Lines and columns count from 1, columns in
/// bytes, as compilers count them; a finding no file holds (the command
/// line's, the configuration's) has neither.
static void collect_tidy_diagnostics(CompilationUnit& unit,
                                     std::vector<worker::TidyDiagnostic>& out) {
    struct File {
        llvm::StringRef path;
        llvm::StringRef content;
        std::vector<std::uint32_t> lines;
    };

    // By the name clang-tidy reports a file by: the one its lookup reached
    // it by.
    llvm::StringMap<File> files;
    auto locate = [&](const clang::tooling::DiagnosticMessage& message) {
        worker::LocatedMessage located{.message = message.Message};
        if(message.FilePath.empty()) {
            return located;
        }
        auto [it, inserted] = files.try_emplace(message.FilePath);
        auto& file = it->second;
        if(inserted) {
            auto& sm = unit.context().getSourceManager();
            auto entry = sm.getFileManager().getOptionalFileRef(message.FilePath);
            file.path = unit.file_path(*entry);
            file.content = sm.getMemoryBufferForFileOrNone(*entry)->getBuffer();
            file.lines = lsp::line_starts(file.content);
        }
        auto position = PositionMap(file.content, file.lines)
                            .position(message.FileOffset, PositionEncoding::UTF8);
        located.file = file.path.str();
        located.line = position->line + 1;
        located.column = position->character + 1;
        return located;
    };

    for(auto& finding: unit.take_tidy_findings()) {
        auto location = locate(finding.Message);
        auto& diagnostic = out.emplace_back(worker::TidyDiagnostic{
            .file = std::move(location.file),
            .line = location.line,
            .column = location.column,
            .error =
                finding.DiagLevel == clang::tooling::Diagnostic::Error || finding.IsWarningAsError,
            .compiler_error =
                finding.DiagLevel == clang::tooling::Diagnostic::Error &&
                llvm::StringRef(finding.DiagnosticName).starts_with("clang-diagnostic-"),
            .message = std::move(location.message),
            .check = std::move(finding.DiagnosticName),
        });
        for(auto& note: finding.Notes) {
            diagnostic.notes.push_back(locate(note));
        }
    }
}

/// The errors a unit compiled with, each at the start of the range clang
/// marks. One no file on disk holds (the command line's, the built-in
/// buffer's, a host fragment's) has no place.
static std::vector<worker::LocatedMessage> located_errors(CompilationUnit& unit) {
    auto& sm = unit.context().getSourceManager();
    std::vector<worker::LocatedMessage> errors;
    for(auto& diag: unit.diagnostics()) {
        if(diag.id.level < DiagnosticLevel::Error) {
            continue;
        }
        auto& error = errors.emplace_back(worker::LocatedMessage{.message = diag.message});
        if(diag.fid.isValid() && !unit.is_builtin_file(diag.fid) && !unit.synthesized(diag.fid)) {
            error.file = unit.file_path(diag.fid).str();
            error.line = sm.getLineNumber(diag.fid, diag.range.begin);
            error.column = sm.getColumnNumber(diag.fid, diag.range.begin);
        }
    }
    return errors;
}

static worker::IncludeTreeResult
    handle_include_tree(const worker::IncludeTreeParams& params,
                        const std::shared_ptr<std::atomic_bool>& stop) {
    LOG_INFO("IncludeTree request: file={}", params.file);
    ScopedTimer timer;
    auto unit = compile({
        .kind = CompilationKind::Preprocess,
        .input = {.arguments = params.arguments,
                  .directory = params.directory,
                  .project_root = params.project_root,
                  .vfs = llvm::makeIntrusiveRefCnt<vfs::View>(params.hidden)},
        .stop = stop,
    });
    if(!unit->completed()) {
        return {.success = false, .error = "preprocessing failed"};
    }
    auto tree = index::IncludeTree::from(*unit);
    LOG_PERF("build",
             "kind=include_tree file={} nodes={} total_ms={}",
             params.file,
             tree.nodes.size(),
             timer.ms());
    return {
        .paths = std::move(tree.paths),
        .content_hashes = std::move(tree.content_hashes),
        .nodes = std::move(tree.nodes),
        .deps = unit->deps(),
        .build_at = unit->build_at().count(),
    };
}

static worker::TURunResult handle_turun(const worker::TURunParams& params,
                                        const std::shared_ptr<std::atomic_bool>& stop) {
    LOG_INFO("TURun request: file={}", params.file);
    ScopedTimer timer;

    // One parse serves every product of the plan. The command-affecting
    // extra args are already in params.arguments (applied at driver
    // resolution); the plan's copies feed the engine's warning-options
    // path only.
    CompilationParams cp{
        .kind = CompilationKind::Whole,
        .input = {.arguments = params.arguments,
                  .directory = params.directory,
                  .project_root = params.project_root,
                  .vfs = llvm::makeIntrusiveRefCnt<vfs::View>(params.hidden)},
        .tidy = params.tidy,
        .stop = stop,
    };
    use_artifacts(cp.input, {params.pch, 0}, params.pcms);

    ScopedTimer compile_timer;
    auto unit = compile(std::move(cp));
    auto compile_ms = compile_timer.ms();
    if(!unit->completed()) {
        LOG_WARN("TU run failed: file={}, {}ms", params.file, timer.ms());
        auto errors = collect_errors(*unit);
        if(errors.empty()) {
            return {false, "TU run compilation failed"};
        }
        // A command clang rejects before parsing: lint reports why, as
        // clang-tidy does.
        worker::TURunResult result{.success = false, .error = llvm::join(errors, "; ")};
        if(params.tidy) {
            for(auto& error: errors) {
                result.tidy_diagnostics.push_back({
                    .file = params.file,
                    .error = true,
                    .compiler_error = true,
                    .message = std::move(error),
                    .check = "clang-diagnostic-error",
                });
            }
        }
        return result;
    }

    // Building and serializing the index costs a large share of the pass;
    // skip it when the cancellation arrived after the parse finished.
    if(stop->load(std::memory_order_relaxed)) {
        return {false, "TU run cancelled"};
    }
    worker::TURunResult result;
    result.success = true;
    ScopedTimer index_timer;
    std::size_t index_bytes = 0;
    if(params.index) {
        auto blob = index::build_tu_index(
            *unit,
            {.known_variants = params.known_variants,
             .known_instantiations = params.known_instantiations,
             .standard_library_instantiations = params.standard_library_instantiations});
        index_bytes = blob.size();
        if(auto error = hand_over_index(std::move(blob), params.index_output_path, result)) {
            return {false, std::move(*error)};
        }
    }
    auto index_ms = index_timer.ms();
    if(params.tidy) {
        collect_tidy_diagnostics(*unit, result.tidy_diagnostics);
    }
    if(params.errors) {
        result.errors = located_errors(*unit);
    }
    if(params.skipped_diagnostics) {
        result.skipped_diagnostics = feature::skipped_diagnostics(*unit);
        result.text_hash = llvm::xxh3_64bits(unit->focus_file().content());
    }

    // AST teardown for a large TU is material work that belongs to this
    // task: sample the total only after the unit is gone, so the logged
    // span covers everything that blocks the worker.
    ScopedTimer teardown_timer;
    unit.reset();
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

static worker::CompletionResult handle_completion(const worker::CompletionParams& params,
                                                  const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    CompilationParams cp{
        .kind = CompilationKind::Completion,
        .input = {.arguments = params.arguments,
                  .directory = params.directory,
                  .vfs = llvm::makeIntrusiveRefCnt<vfs::View>(params.hidden)},
        .completion = {.offset = params.offset},
        .skip_bodies = params.settings.project.skip_header_bodies,
        .stop = stop,
    };
    use_artifacts(cp.input, params.pch, params.pcms);
    cp.input.add_focus(params.file, params.text);
    cp.input.add_synthesized(params.synthesized);

    auto completion = feature::code_complete(std::move(cp),
                                             params.settings.code_completion,
                                             params.settings.documentation,
                                             params.client,
                                             params.encoding,
                                             params.candidates);
    LOG_DEBUG("Completion done: {} items, {}ms", completion.list.items.size(), timer.ms());
    completion.list.is_incomplete = completion.list.is_incomplete || params.more_candidates;

    return {.list = to_raw(completion.list), .include = completion.include};
}

static codec::RawValue handle_signature_help(const worker::SignatureHelpParams& params,
                                             const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    CompilationParams cp{
        .kind = CompilationKind::Completion,
        .input = {.arguments = params.arguments,
                  .directory = params.directory,
                  .vfs = llvm::makeIntrusiveRefCnt<vfs::View>(params.hidden)},
        .completion = {.offset = params.offset},
        .skip_bodies = params.settings.project.skip_header_bodies,
        .stop = stop,
    };
    use_artifacts(cp.input, params.pch, params.pcms);
    cp.input.add_focus(params.file, params.text);
    cp.input.add_synthesized(params.synthesized);

    auto help = feature::signature_help(std::move(cp),
                                        params.settings.signature_help,
                                        params.settings.documentation,
                                        params.selected);
    LOG_DEBUG("SignatureHelp done: {}ms", timer.ms());

    return to_raw(help);
}

static worker::FormatResult handle_format(const worker::FormatParams& params) {
    ScopedTimer timer;

    std::optional<LocalSourceRange> range;
    if(params.range.valid()) {
        range = params.range;
    }

    auto edits = feature::document_format(params.file, params.text, range);
    if(!edits) {
        LOG_DEBUG("Format failed: {}", edits.error());
        return {.error = std::move(edits.error())};
    }
    LOG_DEBUG("Format done: {} edits, {}ms", edits->size(), timer.ms());
    return {.edits = std::move(*edits)};
}

/// How serve() runs a request's work.
struct ServeOptions {
    /// Background work: run it on a thread of its own at lowered priority,
    /// which ends with the request. Lowering the pool thread instead would
    /// leave every later request on it lowered: an unprivileged thread
    /// cannot raise its priority back.
    bool lowered = false;
};

/// Run `work` on a fresh thread of lowered priority and wait for it.
template <typename Work>
static auto run_lowered(Work& work) {
    std::optional<std::invoke_result_t<Work&>> result;
    // As much stack as libuv gives its pool threads; a deep parse needs it.
    llvm::thread thread(std::optional<unsigned>(8u << 20), [&] {
        // Not idle: under a machine kept busy by the user's own build it
        // starves a TU past the build deadline, which then blames the file
        // for a hang.
        sys::set_thread_priority(sys::Priority::BelowNormal);
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
static void
    serve(ipc::BincodePeer& peer, Result cancelled, Handler handler, ServeOptions options = {}) {
    peer.on_request([cancelled, handler, options](RequestContext&,
                                                  const Params& params) -> RequestResult<Params> {
        auto stop = std::make_shared<std::atomic_bool>(false);
        co_return co_await async::queue(
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
    // only one compilation at a time. Must be set before any async::queue call.
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
    purge_freed_memory();

    async::event_loop loop;

    auto transport_result = ipc::StreamTransport::open_stdio(loop);
    if(!transport_result) {
        LOG_ERROR("Failed to open stdio transport");
        return 1;
    }
    (*transport_result)->set_remote_max_payload(ipc::default_max_payload);

    ipc::BincodePeer peer(loop, std::move(*transport_result));
    install_test_gates(peer, loop);

    const worker::ArtifactBuildResult cancelled_build{.success = false, .error = "Build cancelled"};
    serve<worker::BuildPCHParams>(peer, cancelled_build, &handle_build_pch);
    serve<worker::BuildPCMParams>(peer, cancelled_build, &handle_build_pcm);
    serve<worker::TURunParams>(peer,
                               worker::TURunResult{.success = false, .error = "Build cancelled"},
                               &handle_turun,
                               {.lowered = true});
    serve<worker::IncludeTreeParams>(
        peer,
        worker::IncludeTreeResult{.success = false, .error = "Preprocessing cancelled"},
        &handle_include_tree);
    const codec::RawValue cancelled_query{"null"};
    serve<worker::CompletionParams>(peer, worker::CompletionResult{}, &handle_completion);
    serve<worker::SignatureHelpParams>(peer, cancelled_query, &handle_signature_help);
    serve<worker::FormatParams>(
        peer,
        worker::FormatResult{},
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
