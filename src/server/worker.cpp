#include <algorithm>
#include <cstdio>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "eventide/async/loop.h"
#include "eventide/jsonrpc/peer.h"
#include "eventide/jsonrpc/transport.h"
#include "server/protocol.h"
#include "server/runtime.h"

namespace clice::server {

namespace {

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

auto make_completion_result(std::string_view uri, int version, int line, int character)
    -> rpc::RequestTraits<rpc::CompletionParams>::Result {
    rpc::CompletionItem item;
    item.label = "clice::completion";
    item.kind = rpc::CompletionItemKind::Text;
    item.detail = "uri=" + std::string(uri) + ", version=" + std::to_string(version) +
                  ", line=" + std::to_string(line) + ", character=" + std::to_string(character);

    rpc::CompletionList completion{
        .is_incomplete = false,
        .items = {std::move(item)},
    };
    return completion;
}

auto make_signature_help_result(std::string_view uri, int version, int line, int character)
    -> rpc::RequestTraits<rpc::SignatureHelpParams>::Result {
    rpc::ParameterInformation parameter;
    parameter.label = std::string("int value");

    rpc::SignatureInformation signature;
    signature.label = "void clice_signature(" + std::string(uri) + ", " + std::to_string(version) +
                      ", " + std::to_string(line) + ", " + std::to_string(character) + ")";
    signature.parameters = std::vector<rpc::ParameterInformation>{std::move(parameter)};

    rpc::SignatureHelp help{
        .signatures = {std::move(signature)},
        .active_signature = static_cast<rpc::uinteger>(0),
    };
    return help;
}

class WorkerRuntime {
public:
    WorkerRuntime(jsonrpc::Peer& peer, std::size_t document_capacity) :
        peer(peer), document_capacity(std::max<std::size_t>(1, document_capacity)) {
        register_callbacks();
    }

private:
    struct CachedDocument {
        int version = 0;
        std::string text;
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

        peer.on_notification(
            [this](const WorkerEvictParams& params) { evict_document(params.uri); });
    }

    auto on_compile(jsonrpc::RequestContext& context, const WorkerCompileParams& params)
        -> jsonrpc::RequestResult<WorkerCompileParams, WorkerCompileResult> {
        upsert_document(params.uri, params.version, params.text);
        (void)context;

        co_return WorkerCompileResult{
            .uri = params.uri,
            .version = params.version,
            .diagnostics = {},
        };
    }

    auto on_hover(jsonrpc::RequestContext& context, const WorkerHoverParams& params)
        -> jsonrpc::RequestResult<WorkerHoverParams, WorkerHoverResult> {
        upsert_document(params.uri, params.version, params.text);
        (void)context;

        co_return WorkerHoverResult{
            .result = make_hover_result(params.uri, params.version, params.line, params.character),
        };
    }

    auto on_completion(jsonrpc::RequestContext& context, const WorkerCompletionParams& params)
        -> jsonrpc::RequestResult<WorkerCompletionParams, WorkerCompletionResult> {
        upsert_document(params.uri, params.version, params.text);
        (void)context;

        co_return WorkerCompletionResult{
            .result =
                make_completion_result(params.uri, params.version, params.line, params.character),
        };
    }

    auto on_signature_help(jsonrpc::RequestContext& context,
                           const WorkerSignatureHelpParams& params)
        -> jsonrpc::RequestResult<WorkerSignatureHelpParams, WorkerSignatureHelpResult> {
        upsert_document(params.uri, params.version, params.text);
        (void)context;

        co_return WorkerSignatureHelpResult{
            .result = make_signature_help_result(params.uri,
                                                 params.version,
                                                 params.line,
                                                 params.character),
        };
    }

    void upsert_document(std::string_view uri, int version, std::string_view text) {
        auto key = std::string(uri);
        auto doc_iter = documents.find(key);
        if(doc_iter == documents.end()) {
            documents.emplace(key,
                              CachedDocument{
                                  .version = version,
                                  .text = std::string(text),
                              });
        } else {
            doc_iter->second.version = version;
            doc_iter->second.text.assign(text);
        }

        touch_document(key);
        shrink_to_capacity();
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
            documents.erase(victim);
        }
    }

    void evict_document(std::string_view uri) {
        auto key = std::string(uri);
        documents.erase(key);

        auto lru_iter = lru_index.find(key);
        if(lru_iter != lru_index.end()) {
            lru.erase(lru_iter->second);
            lru_index.erase(lru_iter);
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

auto run_worker_mode(const Options& options) -> int {
    et::event_loop loop;
    auto stdio = jsonrpc::StreamTransport::open_stdio(loop);
    if(!stdio) {
        std::fprintf(stderr, "failed to open worker stdio transport: %s\n", stdio.error().c_str());
        return 1;
    }

    jsonrpc::Peer peer(loop, std::move(*stdio));
    WorkerRuntime runtime(peer, options.worker_document_capacity);
    (void)runtime;

    loop.schedule(peer.run());
    return loop.run();
}

}  // namespace clice::server
