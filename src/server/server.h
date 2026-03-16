#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "compile/command.h"
#include "compile/compilation.h"
#include "index/merged_index.h"
#include "index/project_index.h"
#include "server/cache_manager.h"
#include "server/compile_graph.h"
#include "server/config.h"
#include "server/fuzzy_graph.h"
#include "server/options.h"
#include "server/path_pool.h"
#include "server/worker_pool.h"

#include "eventide/async/async.h"
#include "eventide/ipc/peer.h"
#include "eventide/language/protocol.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"

namespace clice {

struct ScanResult;

namespace et = eventide;
namespace protocol = eventide::language::protocol;
using et::ipc::RequestResult;

struct DocumentState {
    std::uint32_t path_id = 0;
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

    // Single-file features
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

    // Cross-file features (index-based)
    RequestResult<protocol::DefinitionParams>
    on_go_to_definition(et::ipc::JsonPeer::RequestContext& ctx,
                        const protocol::DefinitionParams& params);

    RequestResult<protocol::ReferenceParams>
    on_find_references(et::ipc::JsonPeer::RequestContext& ctx,
                       const protocol::ReferenceParams& params);

    RequestResult<protocol::RenameParams>
    on_rename(et::ipc::JsonPeer::RequestContext& ctx,
              const protocol::RenameParams& params);

    RequestResult<protocol::PrepareRenameParams>
    on_prepare_rename(et::ipc::JsonPeer::RequestContext& ctx,
                      const protocol::PrepareRenameParams& params);

    RequestResult<protocol::WorkspaceSymbolParams>
    on_workspace_symbol(et::ipc::JsonPeer::RequestContext& ctx,
                        const protocol::WorkspaceSymbolParams& params);

    RequestResult<protocol::CallHierarchyPrepareParams>
    on_prepare_call_hierarchy(et::ipc::JsonPeer::RequestContext& ctx,
                              const protocol::CallHierarchyPrepareParams& params);

    RequestResult<protocol::CallHierarchyIncomingCallsParams>
    on_incoming_calls(et::ipc::JsonPeer::RequestContext& ctx,
                      const protocol::CallHierarchyIncomingCallsParams& params);

    RequestResult<protocol::CallHierarchyOutgoingCallsParams>
    on_outgoing_calls(et::ipc::JsonPeer::RequestContext& ctx,
                      const protocol::CallHierarchyOutgoingCallsParams& params);

    RequestResult<protocol::TypeHierarchyPrepareParams>
    on_prepare_type_hierarchy(et::ipc::JsonPeer::RequestContext& ctx,
                              const protocol::TypeHierarchyPrepareParams& params);

    RequestResult<protocol::TypeHierarchySubtypesParams>
    on_subtypes(et::ipc::JsonPeer::RequestContext& ctx,
                const protocol::TypeHierarchySubtypesParams& params);

    RequestResult<protocol::TypeHierarchySupertypesParams>
    on_supertypes(et::ipc::JsonPeer::RequestContext& ctx,
                  const protocol::TypeHierarchySupertypesParams& params);

    // Build pipeline
    et::task<> run_build(std::string uri);
    void schedule_build(std::string uri);

    // Indexing
    void update_index(DocumentState& doc);

    // Dependency scanning
    void scan_dependencies(DocumentState& doc);
    void resolve_module_deps(std::uint32_t path_id, const ScanResult& scan);

    // Index queries
    std::optional<index::SymbolHash> symbol_at(std::uint32_t path_id, std::uint32_t offset);
    std::string path_to_uri(llvm::StringRef path);

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

    ServerPathPool path_pool;
    CompileGraph compile_graph{path_pool};
    CacheManager cache_manager;
    FuzzyGraph fuzzy_graph{path_pool};

    // Indexing
    index::ProjectIndex project_index;
    llvm::DenseMap<std::uint32_t, index::MergedIndex> merged_indices;

    // Module name → path_id cache (avoids O(N) scan per import)
    llvm::StringMap<std::uint32_t> module_name_cache;

    // Multi-process workers (optional)
    std::unique_ptr<WorkerPool> workers;
    bool use_workers() const { return workers && workers->has_workers(); }
};

int run_pipe_mode(const Options& options);

}  // namespace clice
