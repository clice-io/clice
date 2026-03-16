#include "server/stateless_worker.h"

#include "compile/compilation.h"
#include "feature/feature.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "eventide/ipc/peer.h"
#include "eventide/language/protocol.h"
#include "eventide/serde/json/serializer.h"

namespace clice {

namespace et = eventide;
namespace lsp = eventide::language::protocol;

StatelessWorker::StatelessWorker(et::event_loop& loop, et::ipc::BincodePeer& peer)
    : loop(loop), peer(peer) {}

void StatelessWorker::register_callbacks() {
    peer.on_request([this](Ctx& ctx, const worker::BuildPCHParams& p) {
        return on_build_pch(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const worker::BuildPCMParams& p) {
        return on_build_pcm(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const worker::CompletionParams& p) {
        return on_completion(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const worker::SignatureHelpParams& p) {
        return on_signature_help(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const worker::IndexParams& p) {
        return on_index(ctx, p);
    });
}

static CompilationParams make_params_from_worker(
    const std::string& file,
    const std::string& directory,
    const std::vector<std::string>& arguments,
    CompilationKind kind) {
    CompilationParams params;
    params.kind = kind;
    params.directory = directory;
    params.arguments_from_database = !directory.empty();

    params.arguments.reserve(arguments.size());
    for(auto& arg : arguments) {
        params.arguments.push_back(arg.c_str());
    }

    return params;
}

static void apply_pcms(CompilationParams& params,
                       const std::vector<std::pair<std::string, std::string>>& pcms) {
    for(auto& [name, path] : pcms) {
        params.pcms.try_emplace(name, path);
    }
}

static void bridge_cancellation(std::shared_ptr<std::atomic_bool>& stop,
                                const et::cancellation_token& token) {
    if(token.cancelled()) {
        stop->store(true);
    }
}

et::task<worker::BuildPCHResult, et::ipc::protocol::Error>
StatelessWorker::on_build_pch(Ctx& ctx, const worker::BuildPCHParams& params) {
    LOG_INFO("Worker: BuildPCH for {}", params.file);

    if(ctx.cancelled()) {
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestCancelled, "request cancelled"));
    }

    auto compile_params = make_params_from_worker(
        params.file, params.directory, params.arguments, CompilationKind::Content);

    compile_params.add_remapped_file(params.file, params.content);
    bridge_cancellation(compile_params.stop, ctx.cancellation);

    PCHInfo pch_info;
    auto unit = compile(compile_params, pch_info);

    worker::BuildPCHResult result;
    if(pch_info.path.empty()) {
        result.success = false;
        result.error = "PCH compilation failed";
    } else {
        result.success = true;
    }

    co_return result;
}

et::task<worker::BuildPCMResult, et::ipc::protocol::Error>
StatelessWorker::on_build_pcm(Ctx& ctx, const worker::BuildPCMParams& params) {
    LOG_INFO("Worker: BuildPCM for {} (module: {})", params.file, params.module_name);

    if(ctx.cancelled()) {
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestCancelled, "request cancelled"));
    }

    auto compile_params = make_params_from_worker(
        params.file, params.directory, params.arguments, CompilationKind::Content);

    apply_pcms(compile_params, params.pcms);
    bridge_cancellation(compile_params.stop, ctx.cancellation);

    PCMInfo pcm_info;
    pcm_info.name = params.module_name;
    auto unit = compile(compile_params, pcm_info);

    worker::BuildPCMResult result;
    if(pcm_info.path.empty()) {
        result.success = false;
        result.error = "PCM compilation failed";
    } else {
        result.success = true;
    }

    co_return result;
}

et::task<worker::CompletionResult, et::ipc::protocol::Error>
StatelessWorker::on_completion(Ctx& ctx, const worker::CompletionParams& params) {
    LOG_INFO("Worker: Completion for {} at {}:{}", params.uri, params.line, params.character);

    if(ctx.cancelled()) {
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestCancelled, "request cancelled"));
    }

    auto compile_params = make_params_from_worker(
        params.uri, params.directory, params.arguments, CompilationKind::Completion);

    compile_params.pch = params.pch;
    apply_pcms(compile_params, params.pcms);
    compile_params.add_remapped_file(params.uri, params.text);

    auto mapper = feature::PositionMapper(params.text, feature::PositionEncoding::UTF16);
    lsp::Position pos;
    pos.line = params.line;
    pos.character = params.character;
    auto offset = mapper.to_offset(pos);

    std::get<0>(compile_params.completion) = params.uri;
    std::get<1>(compile_params.completion) = offset;

    bridge_cancellation(compile_params.stop, ctx.cancellation);

    auto items = feature::code_complete(compile_params);

    lsp::CompletionList list;
    list.is_incomplete = false;
    list.items = std::move(items);

    auto json = eventide::serde::json::to_json(list);
    worker::CompletionResult result;
    result.json_result = json.value_or("null");

    co_return result;
}

et::task<worker::SignatureHelpResult, et::ipc::protocol::Error>
StatelessWorker::on_signature_help(Ctx& ctx, const worker::SignatureHelpParams& params) {
    LOG_INFO("Worker: SignatureHelp for {} at {}:{}", params.uri, params.line, params.character);

    if(ctx.cancelled()) {
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestCancelled, "request cancelled"));
    }

    auto compile_params = make_params_from_worker(
        params.uri, params.directory, params.arguments, CompilationKind::Completion);

    compile_params.pch = params.pch;
    apply_pcms(compile_params, params.pcms);
    compile_params.add_remapped_file(params.uri, params.text);

    auto mapper = feature::PositionMapper(params.text, feature::PositionEncoding::UTF16);
    lsp::Position pos;
    pos.line = params.line;
    pos.character = params.character;
    auto offset = mapper.to_offset(pos);

    std::get<0>(compile_params.completion) = params.uri;
    std::get<1>(compile_params.completion) = offset;

    bridge_cancellation(compile_params.stop, ctx.cancellation);

    auto help = feature::signature_help(compile_params);

    auto json = eventide::serde::json::to_json(help);
    worker::SignatureHelpResult result;
    result.json_result = json.value_or("null");

    co_return result;
}

et::task<worker::IndexResult, et::ipc::protocol::Error>
StatelessWorker::on_index(Ctx& ctx, const worker::IndexParams& params) {
    LOG_INFO("Worker: Index for {}", params.file);

    if(ctx.cancelled()) {
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestCancelled, "request cancelled"));
    }

    auto compile_params = make_params_from_worker(
        params.file, params.directory, params.arguments, CompilationKind::Content);

    apply_pcms(compile_params, params.pcms);
    bridge_cancellation(compile_params.stop, ctx.cancellation);

    auto unit = compile(compile_params);

    worker::IndexResult result;
    result.success = true;
    // TODO: extract TUIndex data from the compilation unit when indexing is fully implemented

    co_return result;
}

int run_stateless_worker_mode(const Options& options) {
    if(auto result = fs::init_resource_dir(options.self_path); !result) {
        LOG_WARN("Failed to init resource dir: {}", result.error());
    }

    et::event_loop loop;

    auto transport = et::ipc::StreamTransport::open_stdio(loop);
    if(!transport) {
        LOG_ERROR("Failed to open stdio transport for worker");
        return 1;
    }

    auto peer = et::ipc::BincodePeer(loop, std::move(*transport));

    StatelessWorker worker(loop, peer);
    worker.register_callbacks();

    loop.schedule(peer.run());
    loop.run();

    return 0;
}

}  // namespace clice
