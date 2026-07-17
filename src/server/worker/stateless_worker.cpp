#include "server/worker/stateless_worker.h"

#include <atomic>
#include <cstdlib>
#include <format>
#include <optional>

#include "compile/compilation.h"
#include "feature/feature.h"
#include "feature/inactive_regions.h"
#include "index/preamble_state.h"
#include "index/tu_index.h"
#include "server/protocol/worker.h"
#include "server/worker/worker_common.h"
#include "support/logging.h"
#include "support/stderr_sink.h"

#include "kota/async/async.h"
#include "kota/ipc/codec/bincode.h"
#include "kota/ipc/peer.h"
#include "kota/ipc/transport.h"
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

/// Extract error messages from compilation diagnostics.
static std::string collect_errors(CompilationUnit& unit) {
    std::string errors;
    for(auto& diag: unit.diagnostics()) {
        if(diag.id.level >= DiagnosticLevel::Error) {
            if(!errors.empty())
                errors += "; ";
            errors += diag.message;
        }
    }
    return errors;
}

/// Serialize the preamble's PreambleState blob (full index + document
/// links + inactive regions) into a string. Runs while the freshly
/// parsed AST is still in memory — the only moment the preamble's index
/// is obtainable without deserializing the whole PCH. The file write
/// happens separately, after the PCH itself is flushed.
static std::string serialize_preamble_state(CompilationUnit& unit, std::uint32_t preamble_bound) {
    auto tu_index = index::TUIndex::build(unit);
    auto links = feature::document_links(unit);
    auto inactive = feature::inactive_regions(unit, {}, 0, preamble_bound);

    std::string blob;
    llvm::raw_string_ostream os(blob);
    index::PreambleState::serialize(unit,
                                    tu_index,
                                    links,
                                    inactive.regions,
                                    inactive.open_stack,
                                    os);
    return blob;
}

/// Write the serialized blob next to the PCH. Returns an error description
/// on failure so the master's anomaly carries the cause.
static std::optional<std::string> write_preamble_state(llvm::StringRef blob,
                                                       llvm::StringRef output_path) {
    std::error_code ec;
    llvm::raw_fd_ostream os(output_path, ec);
    if(ec) {
        auto message =
            std::format("cannot open PreambleState blob {}: {}", output_path, ec.message());
        LOG_ERROR("BuildPCH: {}", message);
        return message;
    }
    os << blob;
    os.flush();
    if(os.has_error()) {
        auto message = std::format("failed writing PreambleState blob {}: {}",
                                   output_path,
                                   os.error().message());
        os.clear_error();
        LOG_ERROR("BuildPCH: {}", message);
        return message;
    }
    return std::nullopt;
}

static worker::BuildResult handle_build_pch(const worker::BuildParams& params,
                                            const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    CompilationParams cp;
    cp.kind = CompilationKind::Preamble;
    fill_args(cp, params.directory, params.arguments);
    cp.add_remapped_file(params.file, params.text, params.preamble_bound);
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
    auto unit = compile(cp, pch_info);
    // A cancelled parse reports !completed(); the extra check catches a
    // cancellation landing between the parse and the serialization, whose
    // blob nobody will read. The tmp file is removed like any failed build.
    bool success = unit.completed() && !stop->load(std::memory_order_relaxed);
    auto build_at = unit.build_at().count();

    std::string errors;
    if(!success)
        errors = collect_errors(unit);

    std::string blob;
    if(success) {
        blob = serialize_preamble_state(unit, params.preamble_bound);
    }

    // Destroy CompilationUnit to flush PCH to disk.
    unit = CompilationUnit(nullptr);

    // Write the blob strictly after the PCH flush: the CacheStore's
    // restart adoption validates a pair by "aux not older than primary"
    // (renames preserve mtimes), so the on-disk order must match the
    // logical one. The PCH is only served together with its blob, so a
    // blob write failure fails the whole build. It is an internal I/O
    // failure, never a user-code problem — must not be downgraded to an
    // expected build failure.
    bool internal_error = false;
    if(success) {
        if(auto error = write_preamble_state(blob, params.index_output_path)) {
            success = false;
            internal_error = true;
            errors = std::move(*error);
        }
    }

    if(success) {
        LOG_INFO("BuildPCH done: file={}, output={}, {}ms", params.file, tmp_path, timer.ms());
        worker::BuildResult result;
        result.success = true;
        result.output_path = tmp_path;
        result.build_at = build_at;
        result.deps = pch_info.deps;
        return result;
    } else {
        LOG_WARN("BuildPCH failed: file={}, {}ms, errors=[{}]", params.file, timer.ms(), errors);
        fs::remove(tmp_path);
        worker::BuildResult result;
        result.success = false;
        result.error = errors.empty() ? "PCH compilation failed" : errors;
        result.has_user_errors = !internal_error && !errors.empty();
        return result;
    }
}

static worker::BuildResult handle_build_pcm(const worker::BuildParams& params,
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
    auto unit = compile(cp, pcm_info);
    bool success = unit.completed() && !stop->load(std::memory_order_relaxed);
    auto build_at = unit.build_at().count();

    std::string errors;
    if(!success)
        errors = collect_errors(unit);

    // TODO: PCM indexing. Unlike the PCH, a PCM is not a transient
    // buffer-derived artifact — module units are ordinary disk files with
    // CDB entries, so their symbols should flow through the normal
    // background-indexing path (no per-blob pair needed).
    unit = CompilationUnit(nullptr);

    if(success) {
        LOG_INFO("BuildPCM done: module={}, {}ms", params.module_name, timer.ms());
        worker::BuildResult result;
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
        worker::BuildResult result;
        result.success = false;
        result.error = errors.empty() ? "PCM compilation failed" : errors;
        result.has_user_errors = !errors.empty();
        return result;
    }
}

static worker::BuildResult handle_index(const worker::BuildParams& params,
                                        const std::shared_ptr<std::atomic_bool>& stop) {
    ScopedTimer timer;

    CompilationParams cp;
    cp.kind = CompilationKind::Indexing;
    fill_args(cp, params.directory, params.arguments);
    for(auto& [name, path]: params.pcms) {
        cp.pcms.try_emplace(name, path);
    }
    cp.stop = stop;

    auto unit = compile(cp);
    if(!unit.completed()) {
        LOG_WARN("Index failed: file={}, {}ms", params.file, timer.ms());
        return {false, "Index compilation failed"};
    }

    // Building and serializing the index costs a large share of the pass;
    // skip both when the cancellation landed after the parse finished.
    if(stop->load(std::memory_order_relaxed)) {
        return {false, "Index cancelled"};
    }
    auto tu_index = index::TUIndex::build(unit);
    std::string serialized;
    llvm::raw_string_ostream os(serialized);
    tu_index.serialize(os);

    LOG_INFO("Index done: file={}, {} symbols, {}ms",
             params.file,
             tu_index.symbols.size(),
             timer.ms());
    worker::BuildResult result;
    result.success = true;
    result.tu_index_data = std::move(serialized);
    return result;
}

static worker::BuildResult handle_completion(const worker::BuildParams& params,
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

    auto items = feature::code_complete(cp);
    LOG_DEBUG("Completion done: {} items, {}ms", items.size(), timer.ms());

    worker::BuildResult result;
    result.result_json = to_raw(items);
    return result;
}

static worker::BuildResult handle_signature_help(const worker::BuildParams& params,
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

    worker::BuildResult result;
    result.result_json = to_raw(help);
    return result;
}

static worker::BuildResult handle_format(const worker::BuildParams& params) {
    ScopedTimer timer;

    std::optional<LocalSourceRange> range;
    if(params.format_range.valid()) {
        range = params.format_range;
    }

    auto edits = feature::document_format(params.file, params.text, range);
    LOG_DEBUG("Format done: {} edits, {}ms", edits.size(), timer.ms());

    worker::BuildResult result;
    result.result_json = to_raw(edits);
    return result;
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

    kota::ipc::BincodePeer peer(loop, std::move(*transport_result));

    peer.on_request([&](RequestContext& ctx,
                        const worker::BuildParams& params) -> RequestResult<worker::BuildParams> {
        using K = worker::BuildKind;
        // A cancellation (peer close, wire-level $/cancelRequest) dequeues
        // work that has not started; work already on the pool thread learns
        // through the hook: the shared flag doubles as CompilationParams::
        // stop, which clang polls after every top-level declaration, so
        // even the parse itself stops instead of running to completion for
        // a result nobody will read.
        auto stop = std::make_shared<std::atomic_bool>(false);
        auto result = co_await kota::queue(
            [&]() -> worker::BuildResult {
                if(stop->load(std::memory_order_relaxed)) {
                    return {false, "Build cancelled"};
                }
                switch(params.kind) {
                    case K::BuildPCH: return handle_build_pch(params, stop);
                    case K::BuildPCM: return handle_build_pcm(params, stop);
                    case K::Index: {
                        ScopedNice guard;
                        return handle_index(params, stop);
                    }
                    case K::Completion: return handle_completion(params, stop);
                    case K::SignatureHelp: return handle_signature_help(params, stop);
                    case K::Format: return handle_format(params);
                }
                return {false, "Unknown build kind"};
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
