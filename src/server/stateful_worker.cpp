#include "server/stateful_worker.h"

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "compile/compilation.h"
#include "eventide/async/async.h"
#include "eventide/ipc/peer.h"
#include "eventide/ipc/transport.h"
#include "eventide/serde/json/serializer.h"
#include "eventide/serde/serde/raw_value.h"
#include "feature/feature.h"
#include "server/protocol.h"
#include "support/logging.h"

#include "llvm/ADT/StringMap.h"

namespace clice {

namespace et = eventide;
using et::ipc::RequestResult;
using RequestContext = et::ipc::BincodePeer::RequestContext;

struct DocumentEntry {
    int version = 0;
    std::string text;
    bool has_ast = false;
    CompilationUnit unit{nullptr};
    std::atomic<bool> dirty{false};

    // Compilation context (from CompileParams)
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, uint32_t> pch;
    llvm::StringMap<std::string> pcms;

    // Per-document serialization mutex
    et::mutex strand;
};

class StatefulWorker {
    et::ipc::BincodePeer& peer;
    std::uint64_t memory_limit;

    llvm::StringMap<std::unique_ptr<DocumentEntry>> documents;

    // LRU tracking — owns keys so they don't dangle after request handler returns
    std::list<std::string> lru;
    llvm::StringMap<std::list<std::string>::iterator> lru_index;

    void touch_lru(llvm::StringRef path) {
        auto it = lru_index.find(path);
        if(it != lru_index.end()) {
            lru.erase(it->second);
        }
        lru.emplace_front(path.str());
        lru_index[path] = lru.begin();
    }

    void shrink_if_over_limit() {
        // TODO: Implement memory-based eviction using memory_limit.
        // For now, cap at a fixed number of documents.
        while(documents.size() > 16 && !lru.empty()) {
            auto path = lru.back();
            lru.pop_back();
            lru_index.erase(path);
            // Send evicted notification to master
            peer.send_notification(worker::EvictedParams{std::string(path)});
            documents.erase(path);
        }
    }

    DocumentEntry& get_or_create(llvm::StringRef path) {
        auto [it, inserted] = documents.try_emplace(path, nullptr);
        if(inserted) {
            it->second = std::make_unique<DocumentEntry>();
        }
        return *it->second;
    }

public:
    StatefulWorker(et::ipc::BincodePeer& peer, std::uint64_t memory_limit) :
        peer(peer), memory_limit(memory_limit) {}

    void register_handlers();
};

void StatefulWorker::register_handlers() {
    // === Compile ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::CompileParams& params) -> RequestResult<worker::CompileParams> {
            auto& doc = get_or_create(params.path);
            doc.version = params.version;
            doc.text = params.text;
            doc.directory = params.directory;
            doc.arguments = params.arguments;
            doc.pch = params.pch;
            doc.pcms.clear();
            for(auto& [name, pcm_path]: params.pcms) {
                doc.pcms.try_emplace(name, pcm_path);
            }

            touch_lru(params.path);

            co_await doc.strand.lock();

            auto compile_result = co_await et::queue([&]() -> worker::CompileResult {
                CompilationParams cp;
                cp.kind = CompilationKind::Content;
                cp.directory = doc.directory;
                for(auto& arg: doc.arguments) {
                    cp.arguments.push_back(arg.c_str());
                }
                if(!doc.pch.first.empty()) {
                    cp.pch = doc.pch;
                }
                cp.add_remapped_file(params.path, doc.text);
                for(auto& entry: doc.pcms) {
                    cp.pcms.try_emplace(entry.getKey(), entry.getValue());
                }

                doc.unit = compile(cp);
                doc.has_ast = true;
                doc.dirty.store(false, std::memory_order_release);

                worker::CompileResult result;
                result.version = doc.version;
                if(doc.unit.completed() || doc.unit.fatal_error()) {
                    auto diags = feature::diagnostics(doc.unit);
                    auto json = et::serde::json::to_json(diags);
                    result.diagnostics = et::serde::RawValue{json ? std::move(*json) : "[]"};
                } else {
                    result.diagnostics = et::serde::RawValue{"[]"};
                }
                result.memory_usage = 0;  // TODO: query actual memory
                return result;
            });

            doc.strand.unlock();
            shrink_if_over_limit();

            co_return compile_result.value();
        });

    // === DocumentUpdate ===
    peer.on_notification([this](const worker::DocumentUpdateParams& params) {
        auto it = documents.find(params.path);
        if(it == documents.end())
            return;

        auto& doc = *it->second;
        doc.version = params.version;
        doc.text = params.text;
        doc.dirty.store(true, std::memory_order_release);
    });

    // === Evict ===
    peer.on_notification([this](const worker::EvictParams& params) {
        auto it = lru_index.find(params.path);
        if(it != lru_index.end()) {
            lru.erase(it->second);
            lru_index.erase(it);
        }
        documents.erase(params.path);
    });

    // === Hover ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::HoverParams& params) -> RequestResult<worker::HoverParams> {
            auto it = documents.find(params.path);
            if(it == documents.end()) {
                co_return et::serde::RawValue{"null"};
            }

            auto& doc = *it->second;
            touch_lru(params.path);

            co_await doc.strand.lock();

            auto result = co_await et::queue([&]() -> et::serde::RawValue {
                if(!doc.has_ast || (!doc.unit.completed() && !doc.unit.fatal_error())) {
                    return et::serde::RawValue{"null"};
                }
                if(doc.dirty.load(std::memory_order_acquire)) {
                    return et::serde::RawValue{"null"};
                }

                auto hover_result = feature::hover(doc.unit, params.offset);
                if(hover_result) {
                    auto json = et::serde::json::to_json(*hover_result);
                    if(json) {
                        return et::serde::RawValue{std::move(*json)};
                    }
                }
                return et::serde::RawValue{"null"};
            });

            doc.strand.unlock();
            co_return result.value();
        });

    // === SemanticTokens ===
    peer.on_request([this](RequestContext& ctx, const worker::SemanticTokensParams& params)
                        -> RequestResult<worker::SemanticTokensParams> {
        auto it = documents.find(params.path);
        if(it == documents.end()) {
            co_return et::serde::RawValue{"null"};
        }

        auto& doc = *it->second;
        touch_lru(params.path);

        co_await doc.strand.lock();

        auto result = co_await et::queue([&]() -> et::serde::RawValue {
            if(!doc.has_ast || (!doc.unit.completed() && !doc.unit.fatal_error())) {
                return et::serde::RawValue{"null"};
            }
            if(doc.dirty.load(std::memory_order_acquire)) {
                return et::serde::RawValue{"null"};
            }

            auto tokens = feature::semantic_tokens(doc.unit);
            auto json = et::serde::json::to_json(tokens);
            if(json) {
                return et::serde::RawValue{std::move(*json)};
            }
            return et::serde::RawValue{"null"};
        });

        doc.strand.unlock();
        co_return result.value();
    });

    // === InlayHints ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::InlayHintsParams& params) -> RequestResult<worker::InlayHintsParams> {
            auto it = documents.find(params.path);
            if(it == documents.end()) {
                co_return et::serde::RawValue{"null"};
            }

            auto& doc = *it->second;
            touch_lru(params.path);

            co_await doc.strand.lock();

            auto result = co_await et::queue([&]() -> et::serde::RawValue {
                if(!doc.has_ast || (!doc.unit.completed() && !doc.unit.fatal_error())) {
                    return et::serde::RawValue{"null"};
                }
                if(doc.dirty.load(std::memory_order_acquire)) {
                    return et::serde::RawValue{"null"};
                }

                LocalSourceRange range{0, static_cast<uint32_t>(doc.text.size())};
                auto hints = feature::inlay_hints(doc.unit, range);
                auto json = et::serde::json::to_json(hints);
                if(json) {
                    return et::serde::RawValue{std::move(*json)};
                }
                return et::serde::RawValue{"null"};
            });

            doc.strand.unlock();
            co_return result.value();
        });

    // === FoldingRange ===
    peer.on_request([this](RequestContext& ctx, const worker::FoldingRangeParams& params)
                        -> RequestResult<worker::FoldingRangeParams> {
        auto it = documents.find(params.path);
        if(it == documents.end()) {
            co_return et::serde::RawValue{"null"};
        }

        auto& doc = *it->second;
        touch_lru(params.path);

        co_await doc.strand.lock();

        auto result = co_await et::queue([&]() -> et::serde::RawValue {
            if(!doc.has_ast || (!doc.unit.completed() && !doc.unit.fatal_error())) {
                return et::serde::RawValue{"null"};
            }
            if(doc.dirty.load(std::memory_order_acquire)) {
                return et::serde::RawValue{"null"};
            }

            auto ranges = feature::folding_ranges(doc.unit);
            auto json = et::serde::json::to_json(ranges);
            if(json) {
                return et::serde::RawValue{std::move(*json)};
            }
            return et::serde::RawValue{"null"};
        });

        doc.strand.unlock();
        co_return result.value();
    });

    // === DocumentSymbol ===
    peer.on_request([this](RequestContext& ctx, const worker::DocumentSymbolParams& params)
                        -> RequestResult<worker::DocumentSymbolParams> {
        auto it = documents.find(params.path);
        if(it == documents.end()) {
            co_return et::serde::RawValue{"null"};
        }

        auto& doc = *it->second;
        touch_lru(params.path);

        co_await doc.strand.lock();

        auto result = co_await et::queue([&]() -> et::serde::RawValue {
            if(!doc.has_ast || (!doc.unit.completed() && !doc.unit.fatal_error())) {
                return et::serde::RawValue{"null"};
            }
            if(doc.dirty.load(std::memory_order_acquire)) {
                return et::serde::RawValue{"null"};
            }

            auto symbols = feature::document_symbols(doc.unit);
            auto json = et::serde::json::to_json(symbols);
            if(json) {
                return et::serde::RawValue{std::move(*json)};
            }
            return et::serde::RawValue{"null"};
        });

        doc.strand.unlock();
        co_return result.value();
    });

    // === DocumentLink ===
    peer.on_request([this](RequestContext& ctx, const worker::DocumentLinkParams& params)
                        -> RequestResult<worker::DocumentLinkParams> {
        auto it = documents.find(params.path);
        if(it == documents.end()) {
            co_return et::serde::RawValue{"null"};
        }

        auto& doc = *it->second;
        touch_lru(params.path);

        co_await doc.strand.lock();

        auto result = co_await et::queue([&]() -> et::serde::RawValue {
            if(!doc.has_ast || (!doc.unit.completed() && !doc.unit.fatal_error())) {
                return et::serde::RawValue{"null"};
            }
            if(doc.dirty.load(std::memory_order_acquire)) {
                return et::serde::RawValue{"null"};
            }

            auto links = feature::document_links(doc.unit);
            auto json = et::serde::json::to_json(links);
            if(json) {
                return et::serde::RawValue{std::move(*json)};
            }
            return et::serde::RawValue{"null"};
        });

        doc.strand.unlock();
        co_return result.value();
    });

    // === CodeAction ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::CodeActionParams& params) -> RequestResult<worker::CodeActionParams> {
            // TODO: Implement code actions
            co_return et::serde::RawValue{"[]"};
        });

    // === GoToDefinition ===
    peer.on_request([this](RequestContext& ctx, const worker::GoToDefinitionParams& params)
                        -> RequestResult<worker::GoToDefinitionParams> {
        // TODO: Implement go-to-definition
        co_return et::serde::RawValue{"[]"};
    });
}

int run_stateful_worker_mode(std::uint64_t memory_limit) {
    logging::stderr_logger("stateful-worker", logging::options);

    et::event_loop loop;

    auto transport_result = et::ipc::StreamTransport::open_stdio(loop);
    if(!transport_result) {
        LOG_ERROR("Failed to open stdio transport");
        return 1;
    }

    et::ipc::BincodePeer peer(loop, std::move(*transport_result));

    StatefulWorker worker(peer, memory_limit);
    worker.register_handlers();

    loop.schedule(peer.run());
    return loop.run();
}

}  // namespace clice
