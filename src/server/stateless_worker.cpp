#include "server/stateless_worker.h"

#include <chrono>

#include "compile/compilation.h"
#include "eventide/async/async.h"
#include "eventide/ipc/json_codec.h"
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

    LOG_INFO("Starting stateless worker");

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
            LOG_INFO("BuildPCH request: file={}", params.file);

            auto result = co_await et::queue([&]() -> worker::BuildPCHResult {
                auto start = std::chrono::steady_clock::now();

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
                    LOG_ERROR("BuildPCH: failed to create temp file");
                    return {false, "Failed to create temporary PCH file"};
                }
                cp.output_file = *tmp;

                PCHInfo pch_info;
                auto unit = compile(cp, pch_info);

                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();

                if(unit.completed()) {
                    LOG_INFO("BuildPCH done: file={}, {}ms", params.file, ms);
                    return {true, ""};
                } else {
                    LOG_WARN("BuildPCH failed: file={}, {}ms", params.file, ms);
                    return {false, "PCH compilation failed"};
                }
            });
            co_return result.value();
        });

    // === BuildPCM ===
    peer.on_request(
        [&](RequestContext& ctx,
            const worker::BuildPCMParams& params) -> RequestResult<worker::BuildPCMParams> {
            LOG_INFO("BuildPCM request: file={}, module={}", params.file, params.module_name);

            auto result = co_await et::queue([&]() -> worker::BuildPCMResult {
                auto start = std::chrono::steady_clock::now();

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
                    LOG_ERROR("BuildPCM: failed to create temp file");
                    return {false, "Failed to create temporary PCM file"};
                }
                cp.output_file = *tmp;

                PCMInfo pcm_info;
                auto unit = compile(cp, pcm_info);

                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();

                if(unit.completed()) {
                    LOG_INFO("BuildPCM done: module={}, {}ms", params.module_name, ms);
                    return {true, ""};
                } else {
                    LOG_WARN("BuildPCM failed: module={}, {}ms", params.module_name, ms);
                    return {false, "PCM compilation failed"};
                }
            });
            co_return result.value();
        });

    // === Completion ===
    peer.on_request(
        [&](RequestContext& ctx,
            const worker::CompletionParams& params) -> RequestResult<worker::CompletionParams> {
            LOG_DEBUG("Completion request: path={}, offset={}", params.path, params.offset);

            auto result = co_await et::queue([&]() -> et::serde::RawValue {
                auto start = std::chrono::steady_clock::now();

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

                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
                LOG_DEBUG("Completion done: {} items, {}ms", items.size(), ms);

                auto json = et::serde::json::to_json<et::ipc::lsp_config>(items);
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
        LOG_DEBUG("SignatureHelp request: path={}, offset={}", params.path, params.offset);

        auto result = co_await et::queue([&]() -> et::serde::RawValue {
            auto start = std::chrono::steady_clock::now();

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

            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
            LOG_DEBUG("SignatureHelp done: {}ms", ms);

            auto json = et::serde::json::to_json<et::ipc::lsp_config>(help);
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
        LOG_INFO("Index request: file={}", params.file);

        auto result = co_await et::queue([&]() -> worker::IndexResult {
            auto start = std::chrono::steady_clock::now();

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

            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();

            if(!unit.completed()) {
                LOG_WARN("Index failed: file={}, {}ms", params.file, ms);
                return {false, "Index compilation failed", ""};
            }

            LOG_INFO("Index done: file={}, {}ms", params.file, ms);
            // TODO: Generate TUIndex from the compilation unit
            return {true, "", ""};
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
