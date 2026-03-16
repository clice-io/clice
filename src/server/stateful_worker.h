#pragma once

#include <list>

#include "compile/compilation.h"
#include "server/options.h"
#include "server/worker_protocol.h"

#include "eventide/async/async.h"
#include "eventide/ipc/peer.h"

#include "llvm/ADT/StringMap.h"

namespace clice {

namespace et = eventide;

class StatefulWorker {
public:
    StatefulWorker(et::event_loop& loop,
                   et::ipc::BincodePeer& peer,
                   std::size_t memory_limit);

    void register_callbacks();

    struct DocumentEntry {
        int version = 0;
        std::string text;
        std::unique_ptr<CompilationUnit> unit;

        std::string directory;
        std::vector<std::string> arguments;
        std::pair<std::string, std::uint32_t> pch;
        std::vector<std::pair<std::string, std::string>> pcms;

        et::mutex strand;
    };

private:
    using Ctx = et::ipc::BincodePeer::RequestContext;

    et::task<worker::DocumentCompileResult, et::ipc::protocol::Error>
    on_compile(Ctx& ctx, const worker::DocumentCompileParams& params);

    et::task<worker::HoverResult, et::ipc::protocol::Error>
    on_hover(Ctx& ctx, const worker::HoverParams& params);

    et::task<worker::SemanticTokensResult, et::ipc::protocol::Error>
    on_semantic_tokens(Ctx& ctx, const worker::SemanticTokensParams& params);

    et::task<worker::DocumentSymbolsResult, et::ipc::protocol::Error>
    on_document_symbols(Ctx& ctx, const worker::DocumentSymbolsParams& params);

    et::task<worker::FoldingRangesResult, et::ipc::protocol::Error>
    on_folding_ranges(Ctx& ctx, const worker::FoldingRangesParams& params);

    et::task<worker::DocumentLinksResult, et::ipc::protocol::Error>
    on_document_links(Ctx& ctx, const worker::DocumentLinksParams& params);

    et::task<worker::InlayHintsResult, et::ipc::protocol::Error>
    on_inlay_hints(Ctx& ctx, const worker::InlayHintsParams& params);

    void on_evict(const worker::EvictParams& params);

    void touch_lru(llvm::StringRef uri);
    void shrink_if_over_limit();

    DocumentEntry* find_document(const std::string& uri);

private:
    et::event_loop& loop;
    et::ipc::BincodePeer& peer;
    std::size_t memory_limit;

    llvm::StringMap<std::unique_ptr<DocumentEntry>> documents;

    std::list<std::string> lru;
    llvm::StringMap<std::list<std::string>::iterator> lru_index;
};

int run_stateful_worker_mode(const Options& options);

}  // namespace clice
