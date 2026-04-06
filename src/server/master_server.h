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
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace et = eventide;
namespace protocol = et::ipc::protocol;

enum class CompletionContext { None, IncludeQuoted, IncludeAngled, Import };

struct PreambleCompletionContext {
    CompletionContext kind = CompletionContext::None;
    std::string prefix;
};

struct DocumentState {
    int version = 0;

    std::string text;

    std::uint64_t generation = 0;

    bool ast_dirty = true;

    /// Non-null while a compile is in flight.  Callers wait on the event;
    /// the compile task runs independently and cannot be cancelled by LSP
    /// $/cancelRequest.
    struct PendingCompile {
        et::event done;
        bool succeeded = false;
    };

    std::shared_ptr<PendingCompile> compiling;
};

/// Context for compiling a header file that lacks its own CDB entry.
struct HeaderFileContext {
    std::uint32_t host_path_id;   // Source file acting as host
    std::string preamble_path;    // Path to generated preamble file on disk
    std::uint64_t preamble_hash;  // Hash of preamble content for staleness
};

/// Two-layer staleness snapshot for compilation artifacts (PCH, AST, etc.).
///
/// Layer 1 (fast): compare each file's current mtime against build_at.
///   If all mtimes <= build_at, the artifact is fresh (zero I/O beyond stat).
///
/// Layer 2 (precise): for files whose mtime changed, re-hash their content
///   and compare against the stored hash.  If the hash matches, the file was
///   "touched" but not actually modified — skip the rebuild.
///
/// This avoids unnecessary recompilation from timestamp-only changes (e.g.
/// git checkout, touch, backup restore) while remaining cheap in the common
/// case where nothing changed.
struct DepsSnapshot {
    /// File path IDs interned via PathPool.
    llvm::SmallVector<std::uint32_t> path_ids;

    /// xxh3_64bits of file content at build time.
    llvm::SmallVector<std::uint64_t> hashes;

    /// time_t when this snapshot was captured.
    std::int64_t build_at = 0;
};

/// Cached PCH state for a single source file.
struct PCHState {
    /// Built PCH file path.
    std::string path;

    /// Preamble byte offset used when building.
    std::uint32_t bound = 0;

    /// xxh3 hash of preamble content.
    std::uint64_t hash = 0;

    /// Dependency snapshot for staleness detection.
    DepsSnapshot deps;

    /// Non-null while a build is in flight.
    std::shared_ptr<et::event> building;
};

/// Cached PCM state for a single module file.
struct PCMState {
    /// Built PCM file path.
    std::string path;

    /// Dependency snapshot for staleness detection.
    DepsSnapshot deps;
};

/// Information about a symbol at a given position.
struct SymbolInfo {
    /// Unique hash identifying this symbol across the project.
    index::SymbolHash hash = 0;

    /// Human-readable symbol name.
    std::string name;

    /// Symbol kind (function, class, variable, etc.).
    SymbolKind kind;

    /// URI of the file containing this symbol.
    std::string uri;

    /// Source range of the symbol's identifier.
    protocol::Range range;
};

/// In-memory index for an open file.  Kept separate from MergedIndex because
/// open files change frequently, are based on unsaved buffer content, and only
/// need to track the main file (headers are covered by PCH/PCM indexing).
struct OpenFileIndex {
    index::FileIndex file_index;
    index::SymbolTable symbols;
    std::string content;  ///< Buffer text at index time (for position mapping).
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
    /// Event loop for scheduling async tasks.
    et::event_loop& loop;

    /// JSON-RPC peer for LSP communication.
    et::ipc::JsonPeer& peer;

    /// Pool of stateful/stateless worker processes.
    WorkerPool pool;

    /// Interning pool for file paths (path string -> uint32_t ID).
    PathPool path_pool;

    /// Current server lifecycle state.
    ServerLifecycle lifecycle = ServerLifecycle::Uninitialized;

    /// Path to the clice binary itself.
    std::string self_path;

    /// Root directory of the opened workspace.
    std::string workspace_root;

    /// User/project configuration.
    CliceConfig config;

    /// Session-specific log directory (e.g. .clice/logs/2026-04-05_10-30-00/).
    std::string session_log_dir;

    /// Compilation database (compile_commands.json).
    CompilationDatabase cdb;

    /// Include/module dependency graph built from fast lexer scanning.
    DependencyGraph dependency_graph;

    /// Module compilation graph (lazy dependency resolution).
    std::unique_ptr<CompileGraph> compile_graph;

    /// path_id -> built PCM output path (set after successful module build).
    llvm::DenseMap<std::uint32_t, std::string> pcm_paths;

    /// path_id -> module name (for files that provide a module interface).
    llvm::DenseMap<std::uint32_t, std::string> path_to_module;

    /// path_id -> cached PCH state (path, preamble hash, deps, build event).
    llvm::DenseMap<std::uint32_t, PCHState> pch_states;

    /// path_id -> cached PCM state (path, deps).
    llvm::DenseMap<std::uint32_t, PCMState> pcm_states;

    // === Index state ===

    /// Global symbol table and path mapping for the project.
    index::ProjectIndex project_index;

    /// Per-file merged index shards (keyed by project-level path_id).
    llvm::DenseMap<std::uint32_t, index::MergedIndex> merged_indices;

    /// Files queued for background indexing (server-level path_ids from CDB).
    std::vector<std::uint32_t> index_queue;

    /// Index of next file to process in index_queue.
    std::size_t index_queue_pos = 0;

    /// Whether background indexing is currently in progress.
    bool indexing_active = false;

    /// Whether a background indexing coroutine has been scheduled (waiting on timer).
    bool indexing_scheduled = false;

    /// Timer for idle-triggered background indexing.
    std::shared_ptr<et::timer> index_idle_timer;

    /// In-memory index for open files (server-level path_id -> OpenFileIndex).
    llvm::DenseMap<std::uint32_t, OpenFileIndex> open_file_indices;

    /// Project-level path_ids of files that have an OpenFileIndex.
    /// Used to skip stale MergedIndex shards during cross-file queries.
    llvm::DenseSet<std::uint32_t> open_proj_path_ids;

    /// path_id -> open document state (text, version, generation, dirty flag).
    llvm::DenseMap<std::uint32_t, DocumentState> documents;

    /// Per-file dependency snapshots from last successful AST compilation.
    llvm::DenseMap<std::uint32_t, DepsSnapshot> ast_deps;

    /// Header context cache: header_path_id -> context
    llvm::DenseMap<std::uint32_t, HeaderFileContext> header_file_contexts;

    /// Active compilation context overrides: path_id -> context_path_id.
    /// When a file has an entry here, it is compiled using the context file's
    /// compile command (e.g. a header compiled through a specific source file).
    llvm::DenseMap<std::uint32_t, std::uint32_t> active_contexts;

    // === Helpers ===

    /// Convert a file:// URI to a local file path.
    std::string uri_to_path(const std::string& uri);

    /// Publish diagnostics to the LSP client.
    void publish_diagnostics(const std::string& uri,
                             int version,
                             const eventide::serde::RawValue& diagnostics_json);

    /// Clear diagnostics for a file (publish empty array).
    void clear_diagnostics(const std::string& uri);

    /// Pull-based compilation entry point. Ensures AST is up-to-date.
    et::task<bool> ensure_compiled(std::uint32_t path_id);

    /// Load CDB and build initial include/module dependency graph.
    et::task<> load_workspace();

    /// Fill compile arguments from CDB for a given file.
    bool fill_compile_args(llvm::StringRef path,
                           std::string& directory,
                           std::vector<std::string>& arguments);

    /// Fill compile arguments using header context (host source's CDB entry
    /// with file path replaced and preamble injected).
    bool fill_header_context_args(llvm::StringRef path,
                                  std::uint32_t path_id,
                                  std::string& directory,
                                  std::vector<std::string>& arguments);

    /// Generate a preamble file for compiling a header in context.
    /// The preamble contains all code from the host source (and intermediate
    /// headers) that comes BEFORE the #include of the target header.
    std::optional<HeaderFileContext> resolve_header_context(std::uint32_t header_path_id);

    /// Build or reuse PCH for a source file. Returns true if PCH is available.
    et::task<bool> ensure_pch(std::uint32_t path_id,
                              llvm::StringRef path,
                              const std::string& text,
                              const std::string& directory,
                              const std::vector<std::string>& arguments);

    /// Compile module dependencies, build/reuse PCH, and fill PCM paths.
    /// Shared preparation step for ensure_compiled() and forward_stateless().
    et::task<bool> ensure_deps(std::uint32_t path_id,
                               llvm::StringRef path,
                               const std::string& text,
                               const std::string& directory,
                               const std::vector<std::string>& arguments,
                               std::pair<std::string, uint32_t>& pch,
                               std::unordered_map<std::string, std::string>& pcms);

    /// Schedule background indexing when idle.
    void schedule_indexing();

    /// Background indexing coroutine: picks files from queue and dispatches to workers.
    et::task<> run_background_indexing();

    /// Merge a TUIndex result into ProjectIndex and MergedIndex shards.
    void merge_index_result(const void* tu_index_data, std::size_t size);

    /// Persist index state to disk.
    void save_index();

    /// Load index state from disk.
    void load_index();

    /// Load PCH/PCM cache metadata from cache.json.
    void load_cache();

    /// Save PCH/PCM cache metadata to cache.json.
    void save_cache();

    /// Clean up stale cache files older than max_age_days.
    void cleanup_cache(int max_age_days = 7);

    // === Include/import completion (handled in master) ===

    /// Detect whether the cursor is on an #include or import line.
    PreambleCompletionContext detect_completion_context(const std::string& text, uint32_t offset);

    /// Complete #include paths by enumerating search directories.
    et::serde::RawValue complete_include(const PreambleCompletionContext& ctx,
                                         llvm::StringRef path);

    /// Complete import module names from the known module map.
    et::serde::RawValue complete_import(const PreambleCompletionContext& ctx);

    // === Feature request forwarding ===

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

    // === Index query helpers ===

    /// Query index for symbol relations (GoToDefinition, FindReferences, etc.).
    RawResult query_index_relations(const std::string& uri,
                                    const protocol::Position& position,
                                    RelationKind kind);

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
    et::task<std::optional<SymbolInfo>>
        resolve_hierarchy_item(const std::string& uri,
                               const protocol::Range& range,
                               const std::optional<protocol::LSPAny>& data);
};

}  // namespace clice
