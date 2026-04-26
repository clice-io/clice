#include "server/worker/stateful_worker.h"

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "compile/compilation.h"
#include "feature/feature.h"
#include "index/tu_index.h"
#include "protocol/worker.h"
#include "server/worker/worker_common.h"
#include "support/logging.h"

#include "kota/async/async.h"
#include "kota/ipc/codec/bincode.h"
#include "kota/ipc/peer.h"
#include "kota/ipc/transport.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"

namespace clice {

using kota::ipc::RequestResult;
using RequestContext = kota::ipc::BincodePeer::RequestContext;

struct DocumentEntry {
    int version = 0;
    std::string text;
    bool has_ast = false;
    CompilationUnit unit{nullptr};
    std::atomic<bool> dirty{false};

    // Signaled when the first compilation completes (has_ast becomes true).
    // Feature handlers co_await this before accessing the AST.
    kota::event ast_ready{false};

    // Compilation context (from CompileParams)
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, uint32_t> pch;
    llvm::StringMap<std::string> pcms;

    // Per-document serialization mutex
    kota::mutex strand;
};

class StatefulWorker {
    kota::ipc::BincodePeer& peer;
    std::uint64_t memory_limit;

    llvm::StringMap<std::shared_ptr<DocumentEntry>> documents;

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
            LOG_DEBUG("Evicting document: {}", path);
            peer.send_notification(worker::EvictedParams{std::string(path)});
            documents.erase(path);
        }
    }

    std::shared_ptr<DocumentEntry> get_or_create(llvm::StringRef path) {
        auto [it, inserted] = documents.try_emplace(path, nullptr);
        if(inserted) {
            it->second = std::make_shared<DocumentEntry>();
            LOG_DEBUG("Created new document entry: {}", path.str());
        }
        return it->second;
    }

    /// Look up document, wait for AST, lock strand, run fn(doc) on thread pool, unlock.
    /// Returns "null" if document not found or AST not usable.
    template <typename F>
    kota::task<kota::codec::RawValue> with_ast(llvm::StringRef path, F&& fn) {
        auto it = documents.find(path);
        if(it == documents.end()) {
            co_return kota::codec::RawValue{"null"};
        }

        // Hold shared_ptr so Evict can't destroy the entry mid-request.
        auto doc = it->second;
        touch_lru(path);

        co_await doc->ast_ready.wait();
        co_await doc->strand.lock();

        auto result = co_await kota::queue([&]() -> kota::codec::RawValue {
            if(!doc->has_ast || (!doc->unit.completed() && !doc->unit.fatal_error()))
                return kota::codec::RawValue{"null"};
            return fn(*doc);
        });

        doc->strand.unlock();
        co_return result.value();
    }

public:
    StatefulWorker(kota::ipc::BincodePeer& peer, std::uint64_t memory_limit) :
        peer(peer), memory_limit(memory_limit) {}

    void register_handlers();
};

void StatefulWorker::register_handlers() {
    // === Compile ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::CompileParams& params) -> RequestResult<worker::CompileParams> {
            LOG_INFO("Compile request: path={}, version={}", params.path, params.version);

            // Hold shared_ptr so Evict can't destroy the entry mid-compile.
            auto doc = get_or_create(params.path);
            touch_lru(params.path);

            co_await doc->strand.lock();

            // Copy params to doc AFTER acquiring the strand lock, so that
            // concurrent Compile requests waiting on the strand don't
            // overwrite our fields before we use them.
            doc->version = params.version;
            doc->text = params.text;
            doc->directory = params.directory;
            doc->arguments = params.arguments;
            doc->pch = params.pch;
            doc->pcms.clear();
            for(auto& [name, pcm_path]: params.pcms) {
                doc->pcms.try_emplace(name, pcm_path);
            }

            auto compile_result = co_await kota::queue([&]() -> worker::CompileResult {
                ScopedTimer timer;

                CompilationParams cp;
                cp.kind = CompilationKind::Content;
                fill_args(cp, doc->directory, doc->arguments);
                if(!doc->pch.first.empty()) {
                    cp.pch = doc->pch;
                }
                cp.add_remapped_file(params.path, doc->text);
                for(auto& entry: doc->pcms) {
                    cp.pcms.try_emplace(entry.getKey(), entry.getValue());
                }

                doc->unit = compile(cp);
                doc->has_ast = true;
                doc->dirty.store(false, std::memory_order_release);

                worker::CompileResult result;
                result.version = doc->version;
                if(doc->unit.completed() || doc->unit.fatal_error()) {
                    auto diags = feature::diagnostics(doc->unit);
                    auto json = kota::codec::json::to_json<kota::ipc::lsp_config>(diags);
                    result.diagnostics = kota::codec::RawValue{json ? std::move(*json) : "[]"};
                    LOG_INFO("Compile done: path={}, {}ms, {} diags, fatal={}",
                             params.path,
                             timer.ms(),
                             diags.size(),
                             doc->unit.fatal_error());
                } else {
                    result.diagnostics = kota::codec::RawValue{"[]"};
                    LOG_WARN("Compile incomplete: path={}, {}ms", params.path, timer.ms());
                }
                result.memory_usage = 0;  // TODO: query actual memory
                if(doc->unit.completed()) {
                    result.deps = doc->unit.deps();

                    // Build index for main file only (interested_only=true).
                    auto tu_index = index::TUIndex::build(doc->unit, true);
                    llvm::raw_string_ostream os(result.tu_index_data);
                    tu_index.serialize(os);
                }
                return result;
            });

            doc->strand.unlock();
            doc->ast_ready.set();
            shrink_if_over_limit();

            co_return compile_result.value();
        });

    // === DocumentUpdate ===
    // Only mark the document dirty — do NOT update doc.text or doc.version
    // here.  The kota::queue compilation work may be reading doc.text on the
    // thread pool concurrently, so writing it from the event loop would be
    // a data race.  The next Compile request will bring the correct text
    // and update it inside the strand lock.
    peer.on_notification([this](const worker::DocumentUpdateParams& params) {
        LOG_TRACE("DocumentUpdate: path={}, version={}", params.path, params.version);

        auto it = documents.find(params.path);
        if(it == documents.end()) {
            LOG_TRACE("DocumentUpdate ignored (not tracked): path={}", params.path);
            return;
        }

        it->second->dirty.store(true, std::memory_order_release);
    });

    // === Evict ===
    peer.on_notification([this](const worker::EvictParams& params) {
        LOG_DEBUG("Evict notification: path={}", params.path);

        auto it = lru_index.find(params.path);
        if(it != lru_index.end()) {
            lru.erase(it->second);
            lru_index.erase(it);
        }
        documents.erase(params.path);
    });

    // === Query (hover, definition, semantic tokens, etc.) ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::QueryParams& params) -> RequestResult<worker::QueryParams> {
            using K = worker::QueryKind;
            switch(params.kind) {
                case K::Hover:
                    co_return co_await with_ast(params.path, [&](DocumentEntry& doc) {
                        auto result = feature::hover(doc.unit, params.offset);
                        return result ? to_raw(*result) : kota::codec::RawValue{"null"};
                    });
                case K::GoToDefinition:
                    // TODO: Implement go-to-definition
                    co_return kota::codec::RawValue{"[]"};
                case K::SemanticTokens:
                    co_return co_await with_ast(params.path, [&](DocumentEntry& doc) {
                        return to_raw(feature::semantic_tokens(doc.unit));
                    });
                case K::InlayHints:
                    co_return co_await with_ast(params.path, [&](DocumentEntry& doc) {
                        auto range = params.range;
                        if(range.begin == static_cast<uint32_t>(-1))
                            range = LocalSourceRange{0, static_cast<uint32_t>(doc.text.size())};
                        return to_raw(feature::inlay_hints(doc.unit, range));
                    });
                case K::FoldingRange:
                    co_return co_await with_ast(params.path, [&](DocumentEntry& doc) {
                        return to_raw(feature::folding_ranges(doc.unit));
                    });
                case K::DocumentSymbol:
                    co_return co_await with_ast(params.path, [&](DocumentEntry& doc) {
                        return to_raw(feature::document_symbols(doc.unit));
                    });
                case K::DocumentLink:
                    co_return co_await with_ast(params.path, [&](DocumentEntry& doc) {
                        return to_raw(feature::document_links(doc.unit));
                    });
                case K::CodeAction:
                    // TODO: Implement code actions
                    co_return kota::codec::RawValue{"[]"};
            }
            co_return kota::codec::RawValue{"null"};
        });
}

int run_stateful_worker_mode(std::uint64_t memory_limit,
                             const std::string& worker_name,
                             const std::string& log_dir) {
    logging::stderr_logger(worker_name, logging::options);
    if(!log_dir.empty()) {
        logging::file_logger(worker_name, log_dir, logging::options);
    }

    LOG_INFO("Starting stateful worker, memory_limit={}MB", memory_limit / (1024 * 1024));

    kota::event_loop loop;

    auto transport_result = kota::ipc::StreamTransport::open_stdio(loop);
    if(!transport_result) {
        LOG_ERROR("Failed to open stdio transport");
        return 1;
    }

    kota::ipc::BincodePeer peer(loop, std::move(*transport_result));

    StatefulWorker worker(peer, memory_limit);
    worker.register_handlers();

    LOG_INFO("Stateful worker ready, waiting for requests");
    loop.schedule(peer.run());
    auto ret = loop.run();
    LOG_INFO("Stateful worker exiting with code {}", ret);
    return ret;
}

}  // namespace clice
