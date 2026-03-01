#include <algorithm>
#include <atomic>
#include <expected>
#include <filesystem>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compile/compilation.h"
#include "compile/preamble.h"
#include "eventide/async/request.h"
#include "feature/feature.h"
#include "server/protocol.h"
#include "server/worker_runtime.h"
#include "support/filesystem.h"

namespace clice::server {

namespace {

namespace fs_std = std::filesystem;

auto make_hover_result(std::string_view uri, int version, int line, int character)
    -> rpc::RequestTraits<rpc::HoverParams>::Result {
    rpc::Hover hover;
    hover.contents = rpc::MarkupContent{
        .kind = rpc::MarkupKind::Plaintext,
        .value = "clice hover snapshot: uri=" + std::string(uri) +
                 ", version=" + std::to_string(version) + ", line=" + std::to_string(line) +
                 ", character=" + std::to_string(character),
    };
    return hover;
}

struct CompileSnapshot {
    std::string uri;
    int version = 0;
    std::string source_path;
    std::string text;
    std::string pch_output_path;
    std::shared_ptr<std::atomic_bool> stop;
    CompileCommand command;
    std::optional<PCHInfo> cached_pch;
};

struct CompileWorkResult {
    WorkerCompileResult response;
    std::optional<PCHInfo> pch;
    std::optional<CompilationUnit> unit;
    bool cancelled = false;
};

auto run_compile_pipeline(CompileSnapshot snapshot) -> CompileWorkResult {
    CompileWorkResult result;
    result.response.uri = snapshot.uri;
    result.response.version = snapshot.version;

    if(snapshot.stop && snapshot.stop->load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return result;
    }

    auto preamble_bound = compute_preamble_bound(snapshot.text);
    auto preamble = snapshot.text.substr(0, preamble_bound);
    auto pch = std::move(snapshot.cached_pch);

    const bool can_reuse_pch = pch.has_value() && preamble_bound > 0 &&
                               pch->path == snapshot.pch_output_path && pch->preamble == preamble;

    if(!can_reuse_pch && preamble_bound > 0) {
        CompilationParams pch_params;
        pch_params.kind = CompilationKind::Preamble;
        pch_params.arguments_from_database = snapshot.command.arguments_from_database;
        pch_params.arguments = snapshot.command.arguments;
        pch_params.directory = snapshot.command.directory;
        pch_params.output_file = snapshot.pch_output_path;
        pch_params.stop = snapshot.stop;
        pch_params.add_remapped_file(snapshot.source_path, snapshot.text, preamble_bound);

        PCHInfo built_pch;
        auto pch_unit = clice::compile(pch_params, built_pch);

        if(pch_unit.cancelled() ||
           (snapshot.stop && snapshot.stop->load(std::memory_order_relaxed))) {
            result.cancelled = true;
            return result;
        }

        if(pch_unit.completed()) {
            pch = std::move(built_pch);
        } else {
            pch.reset();
        }
    } else if(preamble_bound == 0) {
        pch.reset();
    }

    CompilationParams ast_params;
    ast_params.kind = CompilationKind::Content;
    ast_params.arguments_from_database = snapshot.command.arguments_from_database;
    ast_params.arguments = snapshot.command.arguments;
    ast_params.directory = snapshot.command.directory;
    ast_params.stop = snapshot.stop;
    ast_params.add_remapped_file(snapshot.source_path, snapshot.text);

    if(pch && preamble_bound > 0) {
        ast_params.pch = {pch->path, static_cast<std::uint32_t>(pch->preamble.size())};
    }

    auto unit = clice::compile(ast_params);
    if(unit.cancelled() || (snapshot.stop && snapshot.stop->load(std::memory_order_relaxed))) {
        result.cancelled = true;
        return result;
    }

    result.response.diagnostics = feature::diagnostics(unit);
    result.pch = std::move(pch);
    result.unit = std::move(unit);
    return result;
}

class StatefulWorkerRuntime final : public WorkerRuntime {
public:
    StatefulWorkerRuntime(jsonrpc::Peer& peer, std::size_t document_capacity) :
        peer(peer), document_capacity(std::max<std::size_t>(1, document_capacity)) {
        register_callbacks();
    }

private:
    struct CachedDocument {
        int version = 0;
        std::string text;
        std::uint64_t generation = 0;
        std::uint64_t compiled_generation = 0;
        std::shared_ptr<std::atomic_bool> running_stop;
        std::optional<PCHInfo> pch;
        std::optional<CompilationUnit> unit;
        RecipeState recipe_state;
        std::string pch_output_path;
    };

    void register_callbacks() {
        peer.on_request([this](jsonrpc::RequestContext& context, const WorkerCompileParams& params)
                            -> jsonrpc::RequestResult<WorkerCompileParams, WorkerCompileResult> {
            return on_compile(context, params);
        });

        peer.on_request([this](jsonrpc::RequestContext& context, const WorkerHoverParams& params)
                            -> jsonrpc::RequestResult<WorkerHoverParams, WorkerHoverResult> {
            return on_hover(context, params);
        });

        peer.on_notification(
            [this](const WorkerEvictParams& params) { evict_document(params.uri); });
    }

    auto on_compile(jsonrpc::RequestContext& context, const WorkerCompileParams& params)
        -> jsonrpc::RequestResult<WorkerCompileParams, WorkerCompileResult> {
        (void)context;

        auto& document = upsert_document(params.uri, params.version, params.text);
        auto request_generation = document.generation;

        if(document.running_stop) {
            document.running_stop->store(true, std::memory_order_relaxed);
        }

        auto stop = std::make_shared<std::atomic_bool>(false);
        document.running_stop = stop;

        auto command = co_await resolve_compile_command(peer, params.uri, document.recipe_state);
        if(!command) {
            if(document.running_stop == stop) {
                document.running_stop.reset();
            }
            co_return std::unexpected(std::move(command.error()));
        }

        auto prepared = prepare_compile_snapshot(params, document, stop, std::move(*command));
        if(!prepared) {
            if(document.running_stop == stop) {
                document.running_stop.reset();
            }
            co_return std::unexpected(std::move(prepared.error()));
        }

        auto output = std::make_shared<CompileWorkResult>();
        auto queued = co_await et::queue([snapshot = std::move(*prepared), output]() mutable {
            *output = run_compile_pipeline(std::move(snapshot));
        });
        if(queued) {
            auto latest = documents.find(params.uri);
            if(latest != documents.end() && latest->second.running_stop == stop) {
                latest->second.running_stop.reset();
            }
            co_return std::unexpected(std::string("failed to queue compile task: ") +
                                      std::string(queued.message()));
        }

        auto latest = documents.find(params.uri);
        if(latest != documents.end() && latest->second.generation == request_generation) {
            if(latest->second.running_stop == stop) {
                latest->second.running_stop.reset();
            }

            if(!output->cancelled) {
                latest->second.pch = std::move(output->pch);
                latest->second.unit = std::move(output->unit);
                latest->second.compiled_generation = request_generation;
            }
        }

        if(output->cancelled) {
            co_return WorkerCompileResult{
                .uri = params.uri,
                .version = params.version,
                .diagnostics = {},
            };
        }

        co_return std::move(output->response);
    }

    auto on_hover(jsonrpc::RequestContext& context, const WorkerHoverParams& params)
        -> jsonrpc::RequestResult<WorkerHoverParams, WorkerHoverResult> {
        (void)context;

        auto& document = upsert_document(params.uri, params.version, params.text);
        auto request_generation = document.generation;

        auto offset = to_offset_utf16(params.text, params.line, params.character);
        if(document.unit && document.compiled_generation == request_generation) {
            auto real_hover =
                feature::hover(*document.unit, offset, {}, feature::PositionEncoding::UTF16);
            if(real_hover) {
                co_return WorkerHoverResult{
                    .result = std::move(real_hover),
                };
            }
        }

        if(document.running_stop) {
            document.running_stop->store(true, std::memory_order_relaxed);
        }
        auto stop = std::make_shared<std::atomic_bool>(false);
        document.running_stop = stop;

        auto command = co_await resolve_compile_command(peer, params.uri, document.recipe_state);
        if(!command) {
            auto latest = documents.find(params.uri);
            if(latest != documents.end() && latest->second.running_stop == stop) {
                latest->second.running_stop.reset();
            }

            co_return WorkerHoverResult{
                .result =
                    make_hover_result(params.uri, params.version, params.line, params.character),
            };
        }

        auto prepared = prepare_compile_snapshot(
            WorkerCompileParams{
                .uri = params.uri,
                .version = params.version,
                .text = params.text,
            },
            document,
            stop,
            std::move(*command));
        if(!prepared) {
            auto latest = documents.find(params.uri);
            if(latest != documents.end() && latest->second.running_stop == stop) {
                latest->second.running_stop.reset();
            }

            co_return WorkerHoverResult{
                .result =
                    make_hover_result(params.uri, params.version, params.line, params.character),
            };
        }

        auto output = std::make_shared<CompileWorkResult>();
        auto queued = co_await et::queue([snapshot = std::move(*prepared), output]() mutable {
            *output = run_compile_pipeline(std::move(snapshot));
        });
        if(queued) {
            auto latest = documents.find(params.uri);
            if(latest != documents.end() && latest->second.running_stop == stop) {
                latest->second.running_stop.reset();
            }

            co_return WorkerHoverResult{
                .result =
                    make_hover_result(params.uri, params.version, params.line, params.character),
            };
        }

        auto real_hover = rpc::RequestTraits<rpc::HoverParams>::Result(std::nullopt);
        if(!output->cancelled && output->unit) {
            real_hover =
                feature::hover(*output->unit, offset, {}, feature::PositionEncoding::UTF16);
        }

        auto latest = documents.find(params.uri);
        if(latest != documents.end() && latest->second.generation == request_generation) {
            if(latest->second.running_stop == stop) {
                latest->second.running_stop.reset();
            }

            if(!output->cancelled) {
                latest->second.pch = std::move(output->pch);
                latest->second.unit = std::move(output->unit);
                latest->second.compiled_generation = request_generation;
            }
        }

        if(real_hover) {
            co_return WorkerHoverResult{
                .result = std::move(real_hover),
            };
        }

        co_return WorkerHoverResult{
            .result = make_hover_result(params.uri, params.version, params.line, params.character),
        };
    }

    auto upsert_document(std::string_view uri, int version, std::string_view text)
        -> CachedDocument& {
        auto key = std::string(uri);
        auto doc_iter = documents.find(key);
        if(doc_iter == documents.end()) {
            auto [inserted, _] = documents.emplace(key,
                                                   CachedDocument{
                                                       .version = version,
                                                       .text = std::string(text),
                                                       .generation = 1,
                                                   });
            doc_iter = inserted;
        } else {
            auto& document = doc_iter->second;
            const bool version_changed = document.version != version;
            const bool text_changed = document.text != text;
            document.version = version;
            if(text_changed) {
                document.text.assign(text);
            }
            if(version_changed || text_changed) {
                document.generation += 1;
            }
        }

        touch_document(key);
        shrink_to_capacity();
        return documents.find(key)->second;
    }

    auto prepare_compile_snapshot(const WorkerCompileParams& params,
                                  CachedDocument& document,
                                  std::shared_ptr<std::atomic_bool> stop,
                                  CompileCommand command)
        -> std::expected<CompileSnapshot, std::string> {
        if(document.recipe_state.source_path.empty()) {
            return std::unexpected("compile source path is empty");
        }

        if(document.pch_output_path.empty()) {
            auto pch_path = fs::createTemporaryFile("clice-preamble", "pch");
            if(!pch_path) {
                return std::unexpected(std::string("failed to create temporary pch file: ") +
                                       pch_path.error().message());
            }
            document.pch_output_path = std::move(*pch_path);
        }

        CompileSnapshot snapshot{
            .uri = params.uri,
            .version = params.version,
            .source_path = document.recipe_state.source_path,
            .text = params.text,
            .pch_output_path = document.pch_output_path,
            .stop = std::move(stop),
            .command = std::move(command),
            .cached_pch = document.pch,
        };
        return snapshot;
    }

    void touch_document(const std::string& key) {
        auto lru_iter = lru_index.find(key);
        if(lru_iter != lru_index.end()) {
            lru.splice(lru.begin(), lru, lru_iter->second);
            lru_iter->second = lru.begin();
            return;
        }

        lru.push_front(key);
        lru_index.emplace(key, lru.begin());
    }

    void shrink_to_capacity() {
        while(documents.size() > document_capacity && !lru.empty()) {
            auto victim = std::move(lru.back());
            lru.pop_back();
            lru_index.erase(victim);
            cleanup_document(victim);
            documents.erase(victim);
        }
    }

    void evict_document(std::string_view uri) {
        auto key = std::string(uri);
        cleanup_document(key);
        documents.erase(key);

        auto lru_iter = lru_index.find(key);
        if(lru_iter != lru_index.end()) {
            lru.erase(lru_iter->second);
            lru_index.erase(lru_iter);
        }
    }

    void cleanup_document(const std::string& key) {
        auto doc_iter = documents.find(key);
        if(doc_iter == documents.end()) {
            return;
        }

        auto& document = doc_iter->second;
        if(document.running_stop) {
            document.running_stop->store(true, std::memory_order_relaxed);
            document.running_stop.reset();
        }

        if(!document.pch_output_path.empty()) {
            std::error_code ec;
            fs_std::remove(document.pch_output_path, ec);
        }
    }

private:
    jsonrpc::Peer& peer;
    std::size_t document_capacity = 1;
    std::unordered_map<std::string, CachedDocument> documents;
    std::list<std::string> lru;
    std::unordered_map<std::string, std::list<std::string>::iterator> lru_index;
};

}  // namespace

auto make_stateful_worker_runtime(jsonrpc::Peer& peer, std::size_t document_capacity)
    -> std::unique_ptr<WorkerRuntime> {
    return std::make_unique<StatefulWorkerRuntime>(peer, document_capacity);
}

}  // namespace clice::server
