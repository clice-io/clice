#include "server/worker/stateless_worker.h"

#include "compile/compilation.h"
#include "feature/feature.h"
#include "index/tu_index.h"
#include "server/compiler/protocol.h"
#include "server/worker/worker_common.h"
#include "support/logging.h"

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

/// Build a TUIndex, serialize it, and return as a string.
static std::string serialize_tu_index(CompilationUnit& unit, bool interested_only = false) {
    auto tu_index = index::TUIndex::build(unit, interested_only);
    if(!interested_only) {
        tu_index.main_file_index = index::FileIndex();
    }
    std::string serialized;
    llvm::raw_string_ostream os(serialized);
    tu_index.serialize(os);
    return serialized;
}

/// Write compilation output to disk, handling tmp+rename pattern.
/// Returns the final path on success, or empty string on failure.
static std::string finalize_output(const std::string& output_path,
                                   const std::string& tmp_path,
                                   const std::string& file,
                                   const char* label) {
    if(!output_path.empty()) {
        auto ec = fs::rename(tmp_path, output_path);
        if(ec) {
            return output_path;
        } else {
            LOG_WARN("{}: rename {} -> {} failed: {}",
                     label,
                     tmp_path,
                     output_path,
                     ec.error().message());
            return tmp_path;
        }
    }
    return tmp_path;
}

static worker::BuildResult handle_build_pch(const worker::BuildParams& params) {
    ScopedTimer timer;

    CompilationParams cp;
    cp.kind = CompilationKind::Preamble;
    fill_args(cp, params.directory, params.arguments);
    cp.add_remapped_file(params.file, params.text, params.preamble_bound);

    std::string tmp_path;
    bool has_output = !params.output_path.empty();
    if(has_output) {
        tmp_path = params.output_path + ".tmp";
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
    bool success = unit.completed();

    std::string errors;
    if(!success)
        errors = collect_errors(unit);

    std::string tu_index_data;
    std::string pch_links_json;
    if(success) {
        tu_index_data = serialize_tu_index(unit);
        auto links = feature::document_links(unit);
        auto raw = to_raw(links);
        pch_links_json = std::move(raw.data);
    }

    // Destroy CompilationUnit to flush PCH to disk.
    unit = CompilationUnit(nullptr);

    if(success) {
        auto final_path = finalize_output(params.output_path, tmp_path, params.file, "BuildPCH");
        LOG_INFO("BuildPCH done: file={}, output={}, {}ms", params.file, final_path, timer.ms());
        worker::BuildResult result;
        result.success = true;
        result.output_path = std::move(final_path);
        result.deps = pch_info.deps;
        result.tu_index_data = std::move(tu_index_data);
        result.pch_links_json = std::move(pch_links_json);
        return result;
    } else {
        LOG_WARN("BuildPCH failed: file={}, {}ms, errors=[{}]", params.file, timer.ms(), errors);
        fs::remove(tmp_path);
        return {false, errors.empty() ? "PCH compilation failed" : errors};
    }
}

static worker::BuildResult handle_build_pcm(const worker::BuildParams& params) {
    ScopedTimer timer;

    CompilationParams cp;
    cp.kind = CompilationKind::ModuleInterface;
    fill_args(cp, params.directory, params.arguments);
    for(auto& [name, path]: params.pcms) {
        cp.pcms.try_emplace(name, path);
    }

    std::string tmp_path;
    bool has_output = !params.output_path.empty();
    if(has_output) {
        tmp_path = params.output_path + ".tmp";
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
    bool success = unit.completed();

    std::string errors;
    if(!success)
        errors = collect_errors(unit);

    std::string tu_index_data;
    if(success)
        tu_index_data = serialize_tu_index(unit, true);

    unit = CompilationUnit(nullptr);

    if(success) {
        auto final_path = finalize_output(params.output_path, tmp_path, params.file, "BuildPCM");
        LOG_INFO("BuildPCM done: module={}, {}ms", params.module_name, timer.ms());
        worker::BuildResult result;
        result.success = true;
        result.output_path = std::move(final_path);
        result.deps = pcm_info.deps;
        result.tu_index_data = std::move(tu_index_data);
        return result;
    } else {
        LOG_WARN("BuildPCM failed: module={}, {}ms, errors=[{}]",
                 params.module_name,
                 timer.ms(),
                 errors);
        fs::remove(tmp_path);
        return {false, errors.empty() ? "PCM compilation failed" : errors};
    }
}

static worker::BuildResult handle_index(const worker::BuildParams& params) {
    ScopedTimer timer;

    CompilationParams cp;
    cp.kind = CompilationKind::Indexing;
    fill_args(cp, params.directory, params.arguments);
    for(auto& [name, path]: params.pcms) {
        cp.pcms.try_emplace(name, path);
    }

    auto unit = compile(cp);
    if(!unit.completed()) {
        LOG_WARN("Index failed: file={}, {}ms", params.file, timer.ms());
        return {false, "Index compilation failed"};
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

static worker::BuildResult handle_completion(const worker::BuildParams& params) {
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

    auto items = feature::code_complete(cp);
    LOG_DEBUG("Completion done: {} items, {}ms", items.size(), timer.ms());

    worker::BuildResult result;
    result.result_json = to_raw(items);
    return result;
}

static worker::BuildResult handle_signature_help(const worker::BuildParams& params) {
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

    auto help = feature::signature_help(cp);
    LOG_DEBUG("SignatureHelp done: {}ms", timer.ms());

    worker::BuildResult result;
    result.result_json = to_raw(help);
    return result;
}

int run_stateless_worker_mode(const std::string& worker_name, const std::string& log_dir) {
    logging::stderr_logger(worker_name, logging::options);
    if(!log_dir.empty()) {
        logging::file_logger(worker_name, log_dir, logging::options);
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
        auto result = co_await kota::queue([&]() -> worker::BuildResult {
            switch(params.kind) {
                case K::BuildPCH: return handle_build_pch(params);
                case K::BuildPCM: return handle_build_pcm(params);
                case K::Index: {
                    ScopedNice guard;
                    return handle_index(params);
                }
                case K::Completion: return handle_completion(params);
                case K::SignatureHelp: return handle_signature_help(params);
            }
            return {false, "Unknown build kind"};
        });
        co_return result.value();
    });

    LOG_INFO("Stateless worker ready, waiting for requests");
    loop.schedule(peer.run());
    auto ret = loop.run();
    LOG_INFO("Stateless worker exiting with code {}", ret);
    return ret;
}

}  // namespace clice
