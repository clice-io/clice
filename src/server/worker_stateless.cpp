#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compile/compilation.h"
#include "compile/preamble.h"
#include "eventide/async/request.h"
#include "feature/feature.h"
#include "index/tu_index.h"
#include "server/worker_runtime.h"
#include "support/filesystem.h"

namespace clice::server {

namespace {

auto make_completion_result() -> rpc::RequestTraits<rpc::CompletionParams>::Result {
    return rpc::CompletionList{
        .is_incomplete = false,
        .items = {},
    };
}

auto make_signature_help_result() -> rpc::RequestTraits<rpc::SignatureHelpParams>::Result {
    return rpc::SignatureHelp{
        .signatures = {},
    };
}

auto collect_diagnostics(CompilationUnit& unit) -> std::vector<rpc::Diagnostic> {
    if(unit.cancelled() || unit.setup_fail()) {
        return {};
    }
    return feature::diagnostics(unit);
}

auto resolve_output_path(std::string_view requested, const char* prefix, const char* suffix)
    -> std::expected<std::string, std::string> {
    if(!requested.empty()) {
        return std::string(requested);
    }

    auto output_path = fs::createTemporaryFile(prefix, suffix);
    if(!output_path) {
        return std::unexpected("failed to create temporary file: " + output_path.error().message());
    }
    return std::move(*output_path);
}

auto relation_count(const index::FileIndex& file_index) -> std::uint64_t {
    std::uint64_t count = 0;
    for(const auto& [_, relations]: file_index.relations) {
        count += static_cast<std::uint64_t>(relations.size());
    }
    return count;
}

class StatelessWorkerRuntime final : public WorkerRuntime {
public:
    explicit StatelessWorkerRuntime(jsonrpc::Peer& peer) : peer(peer) {
        register_callbacks();
    }

private:
    struct RecipeCache {
        RecipeState recipe_state;
    };

    struct BuildInputs {
        CompileCommand command;
        std::string source_path;
        std::optional<std::string> text;
        std::shared_ptr<std::atomic_bool> stop;
    };

    auto prepare_build_inputs(std::string_view uri, std::optional<std::string> text)
        -> et::task<std::expected<BuildInputs, std::string>> {
        auto& recipe = recipe_cache[std::string(uri)];
        auto command = co_await resolve_compile_command(peer, uri, recipe.recipe_state);
        if(!command) {
            co_return std::unexpected(std::move(command.error()));
        }
        if(recipe.recipe_state.source_path.empty()) {
            co_return std::unexpected("compile source path is empty");
        }

        co_return BuildInputs{
            .command = std::move(*command),
            .source_path = recipe.recipe_state.source_path,
            .text = std::move(text),
            .stop = std::make_shared<std::atomic_bool>(false),
        };
    }

    static void apply_common_compilation_params(CompilationParams& params,
                                                CompilationKind kind,
                                                const BuildInputs& inputs,
                                                bool preamble_only) {
        params.kind = kind;
        params.arguments_from_database = inputs.command.arguments_from_database;
        params.arguments = inputs.command.arguments;
        params.directory = inputs.command.directory;
        params.stop = inputs.stop;
        if(inputs.text) {
            if(preamble_only) {
                auto bound = compute_preamble_bound(*inputs.text);
                params.add_remapped_file(inputs.source_path, *inputs.text, bound);
            } else {
                params.add_remapped_file(inputs.source_path, *inputs.text);
            }
        }
    }

    static void apply_completion_compilation_params(CompilationParams& params,
                                                    const BuildInputs& inputs,
                                                    std::uint32_t offset,
                                                    const std::optional<std::string>& pch_path,
                                                    std::uint32_t pch_preamble_bound) {
        apply_common_compilation_params(params, CompilationKind::Completion, inputs, false);
        params.completion = {inputs.source_path, offset};
        if(pch_path && pch_preamble_bound > 0 && offset > pch_preamble_bound) {
            params.pch = {*pch_path, pch_preamble_bound};
        }
    }

    template <typename Fn>
    auto queue_compile_task(std::string_view task_name, Fn&& task)
        -> et::task<std::expected<void, std::string>> {
        auto queued = co_await et::queue(std::forward<Fn>(task));
        if(queued) {
            co_return std::unexpected("failed to queue " + std::string(task_name) +
                                      " task: " + std::string(queued.message()));
        }
        co_return std::expected<void, std::string>{};
    }

    template <typename Result, typename Fn>
    auto run_build_with_inputs(std::string_view uri,
                               std::optional<std::string> text,
                               std::string_view task_name,
                               Fn&& task) -> et::task<std::expected<Result, std::string>> {
        auto inputs = co_await prepare_build_inputs(uri, std::move(text));
        if(!inputs) {
            co_return std::unexpected(std::move(inputs.error()));
        }

        struct WorkOutput {
            Result result;
            std::string error;
        };

        auto output = std::make_shared<WorkOutput>();
        auto queued = co_await queue_compile_task(
            task_name,
            [output, inputs = std::move(*inputs), task = std::forward<Fn>(task)]() mutable {
                task(output->result, output->error, inputs);
            });
        if(!queued) {
            co_return std::unexpected(std::move(queued.error()));
        }
        if(!output->error.empty()) {
            co_return std::unexpected(std::move(output->error));
        }
        co_return std::move(output->result);
    }

    template <typename FallbackFn, typename QueryFn>
    auto run_completion_like_query(std::string_view uri,
                                   std::string text,
                                   int line,
                                   int character,
                                   std::optional<std::string> pch_path,
                                   std::uint32_t pch_preamble_bound,
                                   std::string_view task_name,
                                   FallbackFn&& fallback,
                                   QueryFn&& query) -> et::task<std::invoke_result_t<FallbackFn&>> {
        using Result = std::invoke_result_t<FallbackFn&>;

        auto offset = to_offset_utf16(text, line, character);
        if(offset > 0 && static_cast<std::size_t>(offset) <= text.size() &&
           text[static_cast<std::size_t>(offset - 1)] == '.') {
            offset -= 1;
        }
        auto inputs = co_await prepare_build_inputs(uri, std::move(text));
        if(!inputs) {
            co_return fallback();
        }

        struct QueryOutput {
            Result result;
        };

        auto output = std::make_shared<QueryOutput>(QueryOutput{
            .result = fallback(),
        });
        auto queued =
            co_await queue_compile_task(task_name,
                                        [output,
                                         inputs = std::move(*inputs),
                                         pch_path = std::move(pch_path),
                                         pch_preamble_bound,
                                         offset,
                                         query = std::forward<QueryFn>(query)]() mutable {
                                            CompilationParams query_params;
                                            apply_completion_compilation_params(query_params,
                                                                                inputs,
                                                                                offset,
                                                                                pch_path,
                                                                                pch_preamble_bound);
                                            query(output->result, query_params);
                                        });
        if(!queued) {
            co_return fallback();
        }

        co_return std::move(output->result);
    }

    void register_callbacks() {
        peer.on_request(
            [this](jsonrpc::RequestContext& context, const WorkerCompletionParams& params)
                -> jsonrpc::RequestResult<WorkerCompletionParams, WorkerCompletionResult> {
                return on_completion(context, params);
            });

        peer.on_request(
            [this](jsonrpc::RequestContext& context, const WorkerSignatureHelpParams& params)
                -> jsonrpc::RequestResult<WorkerSignatureHelpParams, WorkerSignatureHelpResult> {
                return on_signature_help(context, params);
            });

        peer.on_request([this](jsonrpc::RequestContext& context, const WorkerBuildPCHParams& params)
                            -> jsonrpc::RequestResult<WorkerBuildPCHParams, WorkerBuildPCHResult> {
            return on_build_pch(context, params);
        });

        peer.on_request([this](jsonrpc::RequestContext& context, const WorkerBuildPCMParams& params)
                            -> jsonrpc::RequestResult<WorkerBuildPCMParams, WorkerBuildPCMResult> {
            return on_build_pcm(context, params);
        });

        peer.on_request(
            [this](jsonrpc::RequestContext& context, const WorkerBuildIndexParams& params)
                -> jsonrpc::RequestResult<WorkerBuildIndexParams, WorkerBuildIndexResult> {
                return on_build_index(context, params);
            });
    }

    auto on_completion(jsonrpc::RequestContext& context, const WorkerCompletionParams& params)
        -> jsonrpc::RequestResult<WorkerCompletionParams, WorkerCompletionResult> {
        (void)context;

        auto result = co_await run_completion_like_query(
            params.uri,
            params.text,
            params.line,
            params.character,
            params.pch_path,
            params.pch_preamble_bound,
            "completion",
            [] { return make_completion_result(); },
            [](rpc::RequestTraits<rpc::CompletionParams>::Result& result,
               CompilationParams& completion_params) {
                auto items =
                    feature::code_complete(completion_params, {}, feature::PositionEncoding::UTF16);
                if(items.empty()) {
                    return;
                }

                rpc::CompletionList list{
                    .is_incomplete = false,
                    .items = std::move(items),
                };
                result = std::move(list);
            });

        co_return WorkerCompletionResult{
            .result = std::move(result),
        };
    }

    auto on_signature_help(jsonrpc::RequestContext& context,
                           const WorkerSignatureHelpParams& params)
        -> jsonrpc::RequestResult<WorkerSignatureHelpParams, WorkerSignatureHelpResult> {
        (void)context;

        auto result = co_await run_completion_like_query(
            params.uri,
            params.text,
            params.line,
            params.character,
            params.pch_path,
            params.pch_preamble_bound,
            "signature help",
            [] { return make_signature_help_result(); },
            [](rpc::RequestTraits<rpc::SignatureHelpParams>::Result& result,
               CompilationParams& signature_params) {
                auto signature = feature::signature_help(signature_params, {});
                if(signature.signatures.empty()) {
                    return;
                }
                result = std::move(signature);
            });

        co_return WorkerSignatureHelpResult{
            .result = std::move(result),
        };
    }

    auto on_build_pch(jsonrpc::RequestContext& context, const WorkerBuildPCHParams& params)
        -> jsonrpc::RequestResult<WorkerBuildPCHParams, WorkerBuildPCHResult> {
        (void)context;

        auto output_path = params.output_path;
        auto built = co_await run_build_with_inputs<WorkerBuildPCHResult>(
            params.uri,
            params.text,
            "pch build",
            [output_path = std::move(output_path)](WorkerBuildPCHResult& result,
                                                   std::string& error,
                                                   const BuildInputs& inputs) mutable {
                auto resolved = resolve_output_path(output_path, "clice-pch", "pch");
                if(!resolved) {
                    error = std::move(resolved.error());
                    return;
                }

                CompilationParams build_params;
                apply_common_compilation_params(build_params,
                                                CompilationKind::Preamble,
                                                inputs,
                                                true);
                build_params.output_file = *resolved;

                PCHInfo pch;
                auto unit = clice::compile(build_params, pch);
                result.diagnostics = collect_diagnostics(unit);
                result.built = unit.completed();
                if(result.built) {
                    result.output_path = std::move(pch.path);
                }
            });
        if(!built) {
            co_return std::unexpected(std::move(built.error()));
        }

        co_return std::move(*built);
    }

    auto on_build_pcm(jsonrpc::RequestContext& context, const WorkerBuildPCMParams& params)
        -> jsonrpc::RequestResult<WorkerBuildPCMParams, WorkerBuildPCMResult> {
        (void)context;

        auto output_path = params.output_path;
        auto built = co_await run_build_with_inputs<WorkerBuildPCMResult>(
            params.uri,
            params.text,
            "pcm build",
            [output_path = std::move(output_path)](WorkerBuildPCMResult& result,
                                                   std::string& error,
                                                   const BuildInputs& inputs) mutable {
                auto resolved = resolve_output_path(output_path, "clice-pcm", "pcm");
                if(!resolved) {
                    error = std::move(resolved.error());
                    return;
                }

                CompilationParams build_params;
                apply_common_compilation_params(build_params,
                                                CompilationKind::ModuleInterface,
                                                inputs,
                                                false);
                build_params.output_file = *resolved;

                PCMInfo pcm;
                auto unit = clice::compile(build_params, pcm);
                result.diagnostics = collect_diagnostics(unit);
                result.built = unit.completed();
                if(result.built) {
                    result.output_path = std::move(pcm.path);
                    result.source_path = std::move(pcm.srcPath);
                    result.modules = std::move(pcm.mods);
                }
            });
        if(!built) {
            co_return std::unexpected(std::move(built.error()));
        }

        co_return std::move(*built);
    }

    auto on_build_index(jsonrpc::RequestContext& context, const WorkerBuildIndexParams& params)
        -> jsonrpc::RequestResult<WorkerBuildIndexParams, WorkerBuildIndexResult> {
        (void)context;

        auto built = co_await run_build_with_inputs<WorkerBuildIndexResult>(
            params.uri,
            params.text,
            "index build",
            [](WorkerBuildIndexResult& result, std::string&, const BuildInputs& inputs) {
                CompilationParams build_params;
                apply_common_compilation_params(build_params,
                                                CompilationKind::Indexing,
                                                inputs,
                                                false);

                auto unit = clice::compile(build_params);
                result.diagnostics = collect_diagnostics(unit);
                if(!unit.completed()) {
                    result.built = false;
                    return;
                }

                auto built_index = index::TUIndex::build(unit);
                result.built = true;
                result.symbol_count = static_cast<std::uint64_t>(built_index.symbols.size());
                result.main_occurrence_count =
                    static_cast<std::uint64_t>(built_index.main_file_index.occurrences.size());
                result.main_relation_count = relation_count(built_index.main_file_index);
            });
        if(!built) {
            co_return std::unexpected(std::move(built.error()));
        }

        co_return std::move(*built);
    }

private:
    jsonrpc::Peer& peer;
    std::unordered_map<std::string, RecipeCache> recipe_cache;
};

}  // namespace

auto make_stateless_worker_runtime(jsonrpc::Peer& peer) -> std::unique_ptr<WorkerRuntime> {
    return std::make_unique<StatelessWorkerRuntime>(peer);
}

}  // namespace clice::server
