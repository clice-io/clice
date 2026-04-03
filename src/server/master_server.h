#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "command/command.h"
#include "eventide/async/async.h"
#include "eventide/ipc/lsp/protocol.h"
#include "eventide/ipc/peer.h"
#include "eventide/serde/serde/raw_value.h"
#include "index/merged_index.h"
#include "index/project_index.h"
#include "semantic/relation_kind.h"
#include "server/compile_graph.h"
#include "server/config.h"
#include "server/worker_pool.h"
#include "support/path_pool.h"
#include "syntax/dependency_graph.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace et = eventide;
namespace protocol = et::ipc::protocol;

struct DocumentState {
    int version = 0;
    std::string text;
    std::uint64_t generation = 0;
    bool ast_dirty = true;
};

enum class ServerLifecycle : std::uint8_t {
    Uninitialized,
    Initialized,
    Ready,
    ShuttingDown,
    Exited,
};

class MasterServer {
public:
    MasterServer(et::event_loop& loop, et::ipc::JsonPeer& peer, std::string self_path);
    ~MasterServer();

    void register_handlers();

private:
    et::event_loop& loop;
    et::ipc::JsonPeer& peer;
    WorkerPool pool;
    PathPool path_pool;
    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;

    std::string self_path;
    std::string workspace_root;
    CliceConfig config;

    CompilationDatabase cdb;
    DependencyGraph dependency_graph;

    // Module compilation graph (lazy dependency resolution).
    std::unique_ptr<CompileGraph> compile_graph;

    // path_id -> built PCM output path (set after successful module build).
    llvm::DenseMap<std::uint32_t, std::string> pcm_paths;

    // path_id -> module name (for files that provide a module interface).
    llvm::DenseMap<std::uint32_t, std::string> path_to_module;

    // path_id -> built PCH file path.
    llvm::DenseMap<std::uint32_t, std::string> pch_paths;

    // path_id -> preamble bound (byte offset) used when building the PCH.
    llvm::DenseMap<std::uint32_t, std::uint32_t> pch_bounds;

    // path_id -> hash of preamble content at PCH build time (for staleness detection).
    llvm::DenseMap<std::uint32_t, std::uint64_t> pch_hashes;

    // path_id -> in-flight PCH build event (later arrivals co_await the same build).
    llvm::DenseMap<std::uint32_t, std::shared_ptr<et::event>> pch_building;

    // === Index state ===

    // Global symbol table and path mapping for the project.
    index::ProjectIndex project_index;

    // Per-file merged index shards (keyed by project-level path_id).
    llvm::DenseMap<std::uint32_t, index::MergedIndex> merged_indices;

    // Files queued for background indexing (server-level path_ids from CDB).
    std::vector<std::uint32_t> index_queue;

    // Index of next file to process in index_queue.
    std::size_t index_queue_pos = 0;

    // Whether background indexing is currently in progress.
    bool indexing_active = false;

    // Whether a background indexing coroutine has been scheduled (waiting on timer).
    bool indexing_scheduled = false;

    // Timer for idle-triggered background indexing.
    std::shared_ptr<et::timer> index_idle_timer;

    // Document state: path_id -> DocumentState
    llvm::DenseMap<std::uint32_t, DocumentState> documents;

    // Helper: convert URI to file path
    std::string uri_to_path(const std::string& uri);

    // Publish diagnostics to client
    void publish_diagnostics(const std::string& uri,
                             int version,
                             const eventide::serde::RawValue& diagnostics_json);
    void clear_diagnostics(const std::string& uri);

    // Ensure a file has been compiled before servicing feature requests
    et::task<bool> ensure_compiled(std::uint32_t path_id);

    // Load CDB and build initial include graph
    et::task<> load_workspace();

    // Helper: fill compile arguments from CDB into worker params
    bool fill_compile_args(llvm::StringRef path,
                           std::string& directory,
                           std::vector<std::string>& arguments);

    // Build or reuse PCH for a source file. Returns true if PCH is available.
    et::task<bool> ensure_pch(std::uint32_t path_id,
                              llvm::StringRef path,
                              const std::string& text,
                              const std::string& directory,
                              const std::vector<std::string>& arguments);

    // Compile module dependencies, build/reuse PCH, and fill PCM paths into
    // the given fields. Shared by ensure_compiled() and forward_stateless().
    et::task<bool> ensure_deps(std::uint32_t path_id,
                               llvm::StringRef path,
                               const std::string& text,
                               const std::string& directory,
                               const std::vector<std::string>& arguments,
                               std::pair<std::string, uint32_t>& pch,
                               std::unordered_map<std::string, std::string>& pcms);

    // Schedule background indexing when idle.
    void schedule_indexing();

    // Background indexing coroutine: picks files from queue and dispatches to workers.
    et::task<> run_background_indexing();

    // Merge a TUIndex result into ProjectIndex and MergedIndex shards.
    void merge_index_result(const void* tu_index_data, std::size_t size);

    // Persist index state to disk.
    void save_index();

    // Load index state from disk.
    void load_index();

    // Forwarding helpers for feature requests (RawValue passthrough)
    using RawResult = et::task<et::serde::RawValue, et::ipc::Error>;

    /// Forward a simple stateful request (path-only worker params).
    template <typename WorkerParams>
    RawResult forward_stateful(const std::string& uri);

    /// Forward a stateful request with position-to-offset conversion.
    template <typename WorkerParams>
    RawResult forward_stateful(const std::string& uri, const protocol::Position& position);

    /// Forward a stateless request with document content and compile args.
    template <typename WorkerParams>
    RawResult forward_stateless(const std::string& uri, const protocol::Position& position);

    /// Query index for symbol relations (GoToDefinition, FindReferences, etc.).
    /// Returns LSP Location array as RawValue.
    RawResult query_index_relations(const std::string& uri,
                                    const protocol::Position& position,
                                    RelationKind kind);

    /// Information about a symbol at a given position.
    struct SymbolInfo {
        index::SymbolHash hash = 0;
        std::string name;
        SymbolKind kind;
        std::string uri;
        protocol::Range range;
    };

    /// Look up a symbol at a position, returning its hash, name, kind, and range.
    et::task<std::optional<SymbolInfo>>
        lookup_symbol_at_position(const std::string& uri, const protocol::Position& position);

    /// Find the definition location (uri + range) of a symbol by its hash.
    std::optional<protocol::Location> find_symbol_definition_location(index::SymbolHash hash);

    /// Convert clice::SymbolKind to LSP protocol::SymbolKind.
    static protocol::SymbolKind to_lsp_symbol_kind(SymbolKind kind);

    /// Build a CallHierarchyItem from a SymbolInfo.
    protocol::CallHierarchyItem build_call_hierarchy_item(const SymbolInfo& info);

    /// Build a TypeHierarchyItem from a SymbolInfo.
    protocol::TypeHierarchyItem build_type_hierarchy_item(const SymbolInfo& info);

    /// Resolve SymbolInfo from a hierarchy item's stored data (symbol hash).
    /// Falls back to position-based lookup if data is missing.
    et::task<std::optional<SymbolInfo>>
        resolve_hierarchy_item(const std::string& uri,
                               const protocol::Range& range,
                               const std::optional<protocol::LSPAny>& data);
};

}  // namespace clice
