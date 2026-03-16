#include "server/stateful_worker.h"

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

StatefulWorker::StatefulWorker(et::event_loop& loop,
                               et::ipc::BincodePeer& peer,
                               std::size_t memory_limit)
    : loop(loop), peer(peer), memory_limit(memory_limit) {}

void StatefulWorker::register_callbacks() {
    peer.on_request([this](Ctx& ctx, const worker::DocumentCompileParams& p) {
        return on_compile(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const worker::HoverParams& p) {
        return on_hover(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const worker::SemanticTokensParams& p) {
        return on_semantic_tokens(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const worker::DocumentSymbolsParams& p) {
        return on_document_symbols(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const worker::FoldingRangesParams& p) {
        return on_folding_ranges(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const worker::DocumentLinksParams& p) {
        return on_document_links(ctx, p);
    });
    peer.on_request([this](Ctx& ctx, const worker::InlayHintsParams& p) {
        return on_inlay_hints(ctx, p);
    });
    peer.on_notification([this](const worker::EvictParams& p) {
        on_evict(p);
    });
}

static CompilationParams make_params_from_entry(
    const std::string& file,
    StatefulWorker::DocumentEntry& doc,
    CompilationKind kind) {
    CompilationParams params;
    params.kind = kind;
    params.directory = doc.directory;
    params.arguments_from_database = !doc.directory.empty();

    params.arguments.reserve(doc.arguments.size());
    for(auto& arg : doc.arguments) {
        params.arguments.push_back(arg.c_str());
    }

    params.pch = doc.pch;
    for(auto& [name, path] : doc.pcms) {
        params.pcms.try_emplace(name, path);
    }

    return params;
}

et::task<worker::DocumentCompileResult, et::ipc::protocol::Error>
StatefulWorker::on_compile(Ctx& ctx, const worker::DocumentCompileParams& params) {
    LOG_INFO("StatefulWorker: Compile {}", params.uri);

    if(ctx.cancelled()) {
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestCancelled, "request cancelled"));
    }

    auto it = documents.find(params.uri);
    if(it == documents.end()) {
        auto entry = std::make_unique<DocumentEntry>();
        auto [new_it, _] = documents.try_emplace(params.uri, std::move(entry));
        it = new_it;
    }

    auto& doc = *it->second;
    doc.version = params.version;
    doc.text = params.text;
    doc.directory = params.directory;
    doc.arguments = params.arguments;
    doc.pch = params.pch;
    doc.pcms = params.pcms;

    touch_lru(params.uri);

    co_await doc.strand.lock();

    auto compile_params = make_params_from_entry(params.uri, doc, CompilationKind::Content);
    compile_params.clang_tidy = params.clang_tidy;
    compile_params.add_remapped_file(params.uri, doc.text);

    if(ctx.cancelled()) {
        doc.strand.unlock();
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestCancelled, "request cancelled"));
    }

    auto unit = compile(compile_params);
    auto diags = feature::diagnostics(unit);
    doc.unit = std::make_unique<CompilationUnit>(std::move(unit));

    doc.strand.unlock();

    shrink_if_over_limit();

    auto diags_json = eventide::serde::json::to_json(diags);

    worker::DocumentCompileResult result;
    result.diagnostics_json = diags_json.value_or("[]");

    co_return result;
}

et::task<worker::HoverResult, et::ipc::protocol::Error>
StatefulWorker::on_hover(Ctx& ctx, const worker::HoverParams& params) {
    auto* doc = find_document(params.uri);
    if(!doc) {
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestFailed, "document not found"));
    }

    touch_lru(params.uri);
    co_await doc->strand.lock();

    if(!doc->unit) {
        doc->strand.unlock();
        worker::HoverResult result;
        result.json_result = "null";
        co_return result;
    }

    auto mapper = feature::PositionMapper(doc->text, feature::PositionEncoding::UTF16);
    lsp::Position pos;
    pos.line = params.line;
    pos.character = params.character;
    auto offset = mapper.to_offset(pos);

    auto hover = feature::hover(*doc->unit, offset);

    doc->strand.unlock();

    std::string json_str;
    if(hover) {
        auto j = eventide::serde::json::to_json(*hover);
        json_str = j.value_or("null");
    } else {
        json_str = "null";
    }

    worker::HoverResult result;
    result.json_result = std::move(json_str);
    co_return result;
}

et::task<worker::SemanticTokensResult, et::ipc::protocol::Error>
StatefulWorker::on_semantic_tokens(Ctx& ctx, const worker::SemanticTokensParams& params) {
    auto* doc = find_document(params.uri);
    if(!doc) {
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestFailed, "document not found"));
    }

    touch_lru(params.uri);
    co_await doc->strand.lock();

    if(!doc->unit) {
        doc->strand.unlock();
        worker::SemanticTokensResult result;
        result.json_result = "null";
        co_return result;
    }

    auto tokens = feature::semantic_tokens(*doc->unit);

    doc->strand.unlock();

    auto json = eventide::serde::json::to_json(tokens);
    worker::SemanticTokensResult result;
    result.json_result = json.value_or("null");
    co_return result;
}

et::task<worker::DocumentSymbolsResult, et::ipc::protocol::Error>
StatefulWorker::on_document_symbols(Ctx& ctx, const worker::DocumentSymbolsParams& params) {
    auto* doc = find_document(params.uri);
    if(!doc) {
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestFailed, "document not found"));
    }

    touch_lru(params.uri);
    co_await doc->strand.lock();

    if(!doc->unit) {
        doc->strand.unlock();
        worker::DocumentSymbolsResult result;
        result.json_result = "null";
        co_return result;
    }

    auto symbols = feature::document_symbols(*doc->unit);

    doc->strand.unlock();

    auto json = eventide::serde::json::to_json(symbols);
    worker::DocumentSymbolsResult result;
    result.json_result = json.value_or("null");
    co_return result;
}

et::task<worker::FoldingRangesResult, et::ipc::protocol::Error>
StatefulWorker::on_folding_ranges(Ctx& ctx, const worker::FoldingRangesParams& params) {
    auto* doc = find_document(params.uri);
    if(!doc) {
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestFailed, "document not found"));
    }

    touch_lru(params.uri);
    co_await doc->strand.lock();

    if(!doc->unit) {
        doc->strand.unlock();
        worker::FoldingRangesResult result;
        result.json_result = "null";
        co_return result;
    }

    auto ranges = feature::folding_ranges(*doc->unit);

    doc->strand.unlock();

    auto json = eventide::serde::json::to_json(ranges);
    worker::FoldingRangesResult result;
    result.json_result = json.value_or("null");
    co_return result;
}

et::task<worker::DocumentLinksResult, et::ipc::protocol::Error>
StatefulWorker::on_document_links(Ctx& ctx, const worker::DocumentLinksParams& params) {
    auto* doc = find_document(params.uri);
    if(!doc) {
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestFailed, "document not found"));
    }

    touch_lru(params.uri);
    co_await doc->strand.lock();

    if(!doc->unit) {
        doc->strand.unlock();
        worker::DocumentLinksResult result;
        result.json_result = "null";
        co_return result;
    }

    auto links = feature::document_links(*doc->unit);

    doc->strand.unlock();

    auto json = eventide::serde::json::to_json(links);
    worker::DocumentLinksResult result;
    result.json_result = json.value_or("null");
    co_return result;
}

et::task<worker::InlayHintsResult, et::ipc::protocol::Error>
StatefulWorker::on_inlay_hints(Ctx& ctx, const worker::InlayHintsParams& params) {
    auto* doc = find_document(params.uri);
    if(!doc) {
        co_return et::outcome_error(et::ipc::protocol::Error(
            et::ipc::protocol::ErrorCode::RequestFailed, "document not found"));
    }

    touch_lru(params.uri);
    co_await doc->strand.lock();

    if(!doc->unit) {
        doc->strand.unlock();
        worker::InlayHintsResult result;
        result.json_result = "null";
        co_return result;
    }

    lsp::Position start;
    start.line = params.start_line;
    start.character = params.start_character;
    lsp::Position end;
    end.line = params.end_line;
    end.character = params.end_character;

    auto mapper = feature::PositionMapper(doc->text, feature::PositionEncoding::UTF16);
    auto begin_offset = mapper.to_offset(start);
    auto end_offset = mapper.to_offset(end);

    auto hints = feature::inlay_hints(*doc->unit, LocalSourceRange{begin_offset, end_offset});

    doc->strand.unlock();

    auto json = eventide::serde::json::to_json(hints);
    worker::InlayHintsResult result;
    result.json_result = json.value_or("null");
    co_return result;
}

void StatefulWorker::on_evict(const worker::EvictParams& params) {
    LOG_INFO("StatefulWorker: Evict {}", params.uri);

    auto it = documents.find(params.uri);
    if(it != documents.end()) {
        documents.erase(it);
    }

    auto lru_it = lru_index.find(params.uri);
    if(lru_it != lru_index.end()) {
        lru.erase(lru_it->second);
        lru_index.erase(lru_it);
    }
}

void StatefulWorker::touch_lru(llvm::StringRef uri) {
    auto it = lru_index.find(uri);
    if(it != lru_index.end()) {
        lru.erase(it->second);
        lru.push_front(std::string(uri));
        it->second = lru.begin();
    } else {
        lru.push_front(std::string(uri));
        lru_index[uri] = lru.begin();
    }
}

void StatefulWorker::shrink_if_over_limit() {
    // Simple heuristic: limit number of cached ASTs rather than exact memory
    // Each AST can use significant memory; limit based on document count as proxy
    std::size_t max_docs = memory_limit / (256 * 1024 * 1024);
    if(max_docs < 2)
        max_docs = 2;

    while(documents.size() > max_docs && !lru.empty()) {
        auto& oldest_uri = lru.back();

        auto it = documents.find(oldest_uri);
        if(it != documents.end()) {
            documents.erase(it);
        }

        peer.send_notification(worker::EvictedParams{.uri = oldest_uri});

        lru_index.erase(oldest_uri);
        lru.pop_back();
    }
}

StatefulWorker::DocumentEntry* StatefulWorker::find_document(const std::string& uri) {
    auto it = documents.find(uri);
    if(it == documents.end())
        return nullptr;
    return it->second.get();
}

int run_stateful_worker_mode(const Options& options) {
    if(auto result = fs::init_resource_dir(options.self_path); !result) {
        LOG_WARN("Failed to init resource dir: {}", result.error());
    }

    et::event_loop loop;

    auto transport = et::ipc::StreamTransport::open_stdio(loop);
    if(!transport) {
        LOG_ERROR("Failed to open stdio transport for stateful worker");
        return 1;
    }

    auto peer = et::ipc::BincodePeer(loop, std::move(*transport));

    StatefulWorker worker(loop, peer, options.worker_memory_limit);
    worker.register_callbacks();

    loop.schedule(peer.run());
    loop.run();

    return 0;
}

}  // namespace clice
