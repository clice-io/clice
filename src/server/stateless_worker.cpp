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
#include "index/tu_index.h"
#include "server/protocol.h"
#include "support/logging.h"

#include "llvm/Support/raw_ostream.h"

namespace clice {

namespace et = eventide;
using et::ipc::RequestResult;
using RequestContext = et::ipc::BincodePeer::RequestContext;

struct ScopedTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    long long ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start)
            .count();
    }
};

static void fill_args(CompilationParams& cp,
                      const std::string& directory,
                      const std::vector<std::string>& arguments) {
    cp.directory = directory;
    for(auto& arg: arguments) {
        cp.arguments.push_back(arg.c_str());
    }
}

template <typename T>
static et::serde::RawValue to_raw(const T& value) {
    auto json = et::serde::json::to_json<et::ipc::lsp_config>(value);
    return et::serde::RawValue{json ? std::move(*json) : "null"};
}

int run_stateless_worker_mode(const std::string& worker_name, const std::string& log_dir) {
    logging::stderr_logger(worker_name, logging::options);
    if(!log_dir.empty()) {
        logging::file_logger(worker_name, log_dir, logging::options);
    }

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
                ScopedTimer timer;

                CompilationParams cp;
                cp.kind = CompilationKind::Preamble;
                fill_args(cp, params.directory, params.arguments);
                cp.add_remapped_file(params.file, params.content, params.preamble_bound);

                // When output_path is set, write to .tmp then rename — avoids
                // Windows file-lock failures when the target is held by another
                // process.  When empty, fall back to a temporary file.
                std::string tmp_path;
                bool has_output = !params.output_path.empty();
                if(has_output) {
                    tmp_path = params.output_path + ".tmp";
                } else {
                    auto tmp = fs::createTemporaryFile("clice-pch", "pch");
                    if(!tmp) {
                        LOG_ERROR("BuildPCH: failed to create temp file");
                        return {false, "Failed to create temporary PCH file", ""};
                    }
                    tmp_path = *tmp;
                }
                cp.output_file = tmp_path;

                PCHInfo pch_info;
                auto unit = compile(cp, pch_info);
                bool success = unit.completed();

                // Extract error messages before destroying the unit.
                std::string errors;
                if(!success) {
                    for(auto& diag: unit.diagnostics()) {
                        if(diag.id.level >= DiagnosticLevel::Error) {
                            if(!errors.empty())
                                errors += "; ";
                            errors += diag.message;
                        }
                    }
                }

                // Destroy CompilationUnit to flush PCH file to disk
                // (EndSourceFile serializes the AST on destruction).
                unit = CompilationUnit(nullptr);

                // Index preamble headers before destroying the unit.
                std::string tu_index_serialized;
                if(success) {
                    auto tu_index = index::TUIndex::build(unit);
                    // Clear main file index — we only want headers from the PCH.
                    tu_index.main_file_index = index::FileIndex{};
                    llvm::raw_string_ostream os(tu_index_serialized);
                    tu_index.serialize(os);
                }

                if(success) {
                    std::string final_path;
                    if(has_output) {
                        auto ec = fs::rename(tmp_path, params.output_path);
                        if(ec) {
                            final_path = params.output_path;
                        } else {
                            LOG_WARN("BuildPCH: rename {} -> {} failed: {}",
                                     tmp_path,
                                     params.output_path,
                                     ec.error().message());
                            final_path = tmp_path;
                        }
                    } else {
                        final_path = tmp_path;
                    }
                    LOG_INFO("BuildPCH done: file={}, output={}, {}ms",
                             params.file,
                             final_path,
                             timer.ms());
                    worker::BuildPCHResult pch_result{true, "", std::move(final_path)};
                    pch_result.deps = pch_info.deps;
                    pch_result.tu_index_data = std::move(tu_index_serialized);
                    return pch_result;
                } else {
                    LOG_WARN("BuildPCH failed: file={}, {}ms, errors=[{}]",
                             params.file,
                             timer.ms(),
                             errors);
                    fs::remove(tmp_path);
                    return {false, errors.empty() ? "PCH compilation failed" : errors, ""};
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
                        return {false, "Failed to create temporary PCM file", ""};
                    }
                    tmp_path = *tmp;
                }
                cp.output_file = tmp_path;

                PCMInfo pcm_info;
                auto unit = compile(cp, pcm_info);
                bool success = unit.completed();

                std::string errors;
                if(!success) {
                    for(auto& diag: unit.diagnostics()) {
                        if(diag.id.level >= DiagnosticLevel::Error) {
                            if(!errors.empty())
                                errors += "; ";
                            errors += diag.message;
                        }
                    }
                }

                // Index module content before destroying the unit.
                std::string tu_index_serialized;
                if(success) {
                    auto tu_index = index::TUIndex::build(unit);
                    llvm::raw_string_ostream os(tu_index_serialized);
                    tu_index.serialize(os);
                }

                unit = CompilationUnit(nullptr);

                if(success) {
                    std::string final_path = tmp_path;
                    if(has_output) {
                        auto ec = fs::rename(tmp_path, params.output_path);
                        if(ec) {
                            final_path = params.output_path;
                        } else {
                            LOG_WARN("BuildPCM: rename {} -> {} failed: {}",
                                     tmp_path,
                                     params.output_path,
                                     ec.error().message());
                        }
                    }
                    LOG_INFO("BuildPCM done: module={}, {}ms", params.module_name, timer.ms());
                    worker::BuildPCMResult pcm_result{true, "", std::move(final_path)};
                    pcm_result.deps = pcm_info.deps;
                    pcm_result.tu_index_data = std::move(tu_index_serialized);
                    return pcm_result;
                } else {
                    LOG_WARN("BuildPCM failed: module={}, {}ms, errors=[{}]",
                             params.module_name,
                             timer.ms(),
                             errors);
                    fs::remove(tmp_path);
                    return {false, errors.empty() ? "PCM compilation failed" : errors, ""};
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
                cp.add_remapped_file(params.path, params.text);
                cp.completion = {params.path, params.offset};

                auto items = feature::code_complete(cp);
                LOG_DEBUG("Completion done: {} items, {}ms", items.size(), timer.ms());
                return to_raw(items);
            });
            co_return result.value();
        });

    // === SignatureHelp ===
    peer.on_request([&](RequestContext& ctx, const worker::SignatureHelpParams& params)
                        -> RequestResult<worker::SignatureHelpParams> {
        LOG_DEBUG("SignatureHelp request: path={}, offset={}", params.path, params.offset);

        auto result = co_await et::queue([&]() -> et::serde::RawValue {
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
            cp.add_remapped_file(params.path, params.text);
            cp.completion = {params.path, params.offset};

            auto help = feature::signature_help(cp);
            LOG_DEBUG("SignatureHelp done: {}ms", timer.ms());
            return to_raw(help);
        });
        co_return result.value();
    });

    // === Index ===
    peer.on_request([&](RequestContext& ctx,
                        const worker::IndexParams& params) -> RequestResult<worker::IndexParams> {
        LOG_INFO("Index request: file={}", params.file);

        auto result = co_await et::queue([&]() -> worker::IndexResult {
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
                return {false, "Index compilation failed", ""};
            }

            auto tu_index = index::TUIndex::build(unit);

            std::string serialized;
            llvm::raw_string_ostream os(serialized);
            tu_index.serialize(os);

            LOG_INFO("Index done: file={}, {} symbols, {}ms",
                     params.file,
                     tu_index.symbols.size(),
                     timer.ms());
            return {true, "", std::move(serialized)};
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
