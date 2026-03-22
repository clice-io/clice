#include "server/stateless_worker.h"

#include "compile/compilation.h"
#include "eventide/async/async.h"
#include "eventide/ipc/peer.h"
#include "eventide/ipc/transport.h"
#include "eventide/serde/json/serializer.h"
#include "eventide/serde/serde/raw_value.h"
#include "feature/feature.h"
#include "server/protocol.h"
#include "support/logging.h"

namespace clice {

namespace et = eventide;
using et::ipc::RequestResult;
using RequestContext = et::ipc::BincodePeer::RequestContext;

int run_stateless_worker_mode() {
    logging::stderr_logger("stateless-worker", logging::options);

    et::event_loop loop;

    auto transport_result = et::ipc::StreamTransport::open_stdio(loop);
    if(!transport_result) {
        LOG_ERROR("Failed to open stdio transport");
        return 1;
    }

    et::ipc::BincodePeer peer(loop, std::move(*transport_result));

    // === BuildPCH ===
    peer.on_request(
        [&](RequestContext& ctx,
            const worker::BuildPCHParams& params) -> RequestResult<worker::BuildPCHParams> {
            auto result = co_await et::queue([&]() -> worker::BuildPCHResult {
                CompilationParams cp;
                cp.kind = CompilationKind::Preamble;
                cp.directory = params.directory;
                for(auto& arg: params.arguments) {
                    cp.arguments.push_back(arg.c_str());
                }
                cp.add_remapped_file(params.file, params.content);

                // Generate a temporary file path for the PCH output.
                auto tmp = fs::createTemporaryFile("clice-pch", "pch");
                if(!tmp) {
                    return {false, "Failed to create temporary PCH file"};
                }
                cp.output_file = *tmp;

                PCHInfo pch_info;
                auto unit = compile(cp, pch_info);

                if(unit.completed()) {
                    return {true, ""};
                } else {
                    return {false, "PCH compilation failed"};
                }
            });
            co_return result.value();
        });

    // === BuildPCM ===
    peer.on_request(
        [&](RequestContext& ctx,
            const worker::BuildPCMParams& params) -> RequestResult<worker::BuildPCMParams> {
            auto result = co_await et::queue([&]() -> worker::BuildPCMResult {
                CompilationParams cp;
                cp.kind = CompilationKind::ModuleInterface;
                cp.directory = params.directory;
                for(auto& arg: params.arguments) {
                    cp.arguments.push_back(arg.c_str());
                }
                for(auto& [name, path]: params.pcms) {
                    cp.pcms.try_emplace(name, path);
                }

                // Generate a temporary file path for the PCM output.
                auto tmp = fs::createTemporaryFile("clice-pcm", "pcm");
                if(!tmp) {
                    return {false, "Failed to create temporary PCM file"};
                }
                cp.output_file = *tmp;

                PCMInfo pcm_info;
                auto unit = compile(cp, pcm_info);

                if(unit.completed()) {
                    return {true, ""};
                } else {
                    return {false, "PCM compilation failed"};
                }
            });
            co_return result.value();
        });

    // === Completion ===
    peer.on_request(
        [&](RequestContext& ctx,
            const worker::CompletionParams& params) -> RequestResult<worker::CompletionParams> {
            auto result = co_await et::queue([&]() -> et::serde::RawValue {
                CompilationParams cp;
                cp.kind = CompilationKind::Completion;
                cp.directory = params.directory;
                for(auto& arg: params.arguments) {
                    cp.arguments.push_back(arg.c_str());
                }
                if(!params.pch.first.empty()) {
                    cp.pch = params.pch;
                }
                for(auto& [name, path]: params.pcms) {
                    cp.pcms.try_emplace(name, path);
                }
                cp.add_remapped_file(params.path, params.text);
                cp.completion = {params.path, params.offset};

                auto items = feature::code_complete(cp);

                auto json = et::serde::json::to_json(items);
                if(json) {
                    return et::serde::RawValue{std::move(*json)};
                }
                return et::serde::RawValue{"[]"};
            });
            co_return result.value();
        });

    // === SignatureHelp ===
    peer.on_request([&](RequestContext& ctx, const worker::SignatureHelpParams& params)
                        -> RequestResult<worker::SignatureHelpParams> {
        auto result = co_await et::queue([&]() -> et::serde::RawValue {
            CompilationParams cp;
            cp.kind = CompilationKind::Completion;
            cp.directory = params.directory;
            for(auto& arg: params.arguments) {
                cp.arguments.push_back(arg.c_str());
            }
            if(!params.pch.first.empty()) {
                cp.pch = params.pch;
            }
            for(auto& [name, path]: params.pcms) {
                cp.pcms.try_emplace(name, path);
            }
            cp.add_remapped_file(params.path, params.text);
            cp.completion = {params.path, params.offset};

            auto help = feature::signature_help(cp);

            auto json = et::serde::json::to_json(help);
            if(json) {
                return et::serde::RawValue{std::move(*json)};
            }
            return et::serde::RawValue{"null"};
        });
        co_return result.value();
    });

    // === Index ===
    peer.on_request([&](RequestContext& ctx,
                        const worker::IndexParams& params) -> RequestResult<worker::IndexParams> {
        auto result = co_await et::queue([&]() -> worker::IndexResult {
            CompilationParams cp;
            cp.kind = CompilationKind::Indexing;
            cp.directory = params.directory;
            for(auto& arg: params.arguments) {
                cp.arguments.push_back(arg.c_str());
            }
            for(auto& [name, path]: params.pcms) {
                cp.pcms.try_emplace(name, path);
            }

            auto unit = compile(cp);
            if(!unit.completed()) {
                return {false, "Index compilation failed", ""};
            }

            // TODO: Generate TUIndex from the compilation unit
            return {true, "", ""};
        });
        co_return result.value();
    });

    loop.schedule(peer.run());
    return loop.run();
}

}  // namespace clice
