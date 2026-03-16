#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "compile/command.h"
#include "compile/compilation.h"
#include "server/config.h"
#include "server/options.h"

#include "eventide/async/async.h"
#include "eventide/ipc/peer.h"
#include "eventide/language/protocol.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"

namespace clice {

namespace et = eventide;
namespace protocol = eventide::language::protocol;
using et::ipc::RequestResult;

struct DocumentState {
    std::string uri;
    std::string path;
    int version = 0;
    std::string text;
    std::uint64_t generation = 0;

    bool build_requested = false;
    bool build_running = false;

    std::unique_ptr<CompilationUnit> unit;
    std::unique_ptr<et::event> build_complete;
};

class Server {
public:
    Server(et::event_loop& loop,
           et::ipc::JsonPeer& peer,
           const Options& options);

    void register_callbacks();

private:
    // LSP lifecycle
    RequestResult<protocol::InitializeParams>
    on_initialize(et::ipc::JsonPeer::RequestContext& ctx,
                  const protocol::InitializeParams& params);

    et::task<protocol::null, et::ipc::protocol::Error>
    on_shutdown(et::ipc::JsonPeer::RequestContext& ctx,
                const protocol::Value& params);

    void on_exit(const protocol::Value& params);
    void on_initialized(const protocol::Value& params);

    // Document sync
    void on_did_open(const protocol::DidOpenTextDocumentParams& params);
    void on_did_change(const protocol::DidChangeTextDocumentParams& params);
    void on_did_save(const protocol::DidSaveTextDocumentParams& params);
    void on_did_close(const protocol::DidCloseTextDocumentParams& params);

    // Feature requests
    RequestResult<protocol::HoverParams>
    on_hover(et::ipc::JsonPeer::RequestContext& ctx,
             const protocol::HoverParams& params);

    RequestResult<protocol::CompletionParams>
    on_completion(et::ipc::JsonPeer::RequestContext& ctx,
                  const protocol::CompletionParams& params);

    RequestResult<protocol::SignatureHelpParams>
    on_signature_help(et::ipc::JsonPeer::RequestContext& ctx,
                      const protocol::SignatureHelpParams& params);

    RequestResult<protocol::DocumentFormattingParams>
    on_formatting(et::ipc::JsonPeer::RequestContext& ctx,
                  const protocol::DocumentFormattingParams& params);

    RequestResult<protocol::SemanticTokensParams>
    on_semantic_tokens(et::ipc::JsonPeer::RequestContext& ctx,
                       const protocol::SemanticTokensParams& params);

    RequestResult<protocol::DocumentSymbolParams>
    on_document_symbols(et::ipc::JsonPeer::RequestContext& ctx,
                        const protocol::DocumentSymbolParams& params);

    RequestResult<protocol::FoldingRangeParams>
    on_folding_ranges(et::ipc::JsonPeer::RequestContext& ctx,
                      const protocol::FoldingRangeParams& params);

    RequestResult<protocol::DocumentLinkParams>
    on_document_links(et::ipc::JsonPeer::RequestContext& ctx,
                      const protocol::DocumentLinkParams& params);

    RequestResult<protocol::InlayHintParams>
    on_inlay_hints(et::ipc::JsonPeer::RequestContext& ctx,
                   const protocol::InlayHintParams& params);

    // Build
    et::task<> run_build(std::string uri);
    void schedule_build(std::string uri);

    // Helpers
    std::string uri_to_path(const std::string& uri);
    DocumentState* find_document(const std::string& uri);
    void publish_diagnostics(const std::string& uri,
                             const std::vector<protocol::Diagnostic>& diags);

    CompilationParams make_compile_params(DocumentState& doc, CompilationKind kind);

private:
    et::event_loop& loop;
    et::ipc::JsonPeer& peer;
    Options options;

    enum class State { Uninitialized, Running, ShuttingDown };
    State state = State::Uninitialized;

    std::string workspace_root;
    Config config;
    CompilationDatabase cdb;

    llvm::StringMap<DocumentState> documents;
};

int run_pipe_mode(const Options& options);

}  // namespace clice
