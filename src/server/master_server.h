#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "command/command.h"
#include "eventide/async/async.h"
#include "eventide/ipc/lsp/protocol.h"
#include "eventide/ipc/peer.h"
#include "eventide/serde/serde/raw_value.h"
#include "server/compile_graph.h"
#include "server/config.h"
#include "server/indexer.h"
#include "server/worker_pool.h"
#include "support/path_pool.h"
#include "syntax/dependency_graph.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
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

    /// Cross-file symbol index (ProjectIndex, MergedIndex shards, open file indices).
    Indexer indexer;

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

    // === Indexing scheduling state ===

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

    // === Document state ===

    /// path_id -> open document state (text, version, generation, dirty flag).
    llvm::DenseMap<std::uint32_t, DocumentState> documents;

    /// Per-file dependency snapshots from last successful AST compilation.
    llvm::DenseMap<std::uint32_t, DepsSnapshot> ast_deps;

    /// Header context cache: header_path_id -> context
    llvm::DenseMap<std::uint32_t, HeaderFileContext> header_file_contexts;

    /// Active compilation context overrides: path_id -> context_path_id.
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

    /// Fill compile arguments using header context.
    bool fill_header_context_args(llvm::StringRef path,
                                  std::uint32_t path_id,
                                  std::string& directory,
                                  std::vector<std::string>& arguments);

    /// Generate a preamble file for compiling a header in context.
    std::optional<HeaderFileContext> resolve_header_context(std::uint32_t header_path_id);

    /// Build or reuse PCH for a source file.
    et::task<bool> ensure_pch(std::uint32_t path_id,
                              llvm::StringRef path,
                              const std::string& text,
                              const std::string& directory,
                              const std::vector<std::string>& arguments);

    /// Compile module dependencies, build/reuse PCH, and fill PCM paths.
    et::task<bool> ensure_deps(std::uint32_t path_id,
                               llvm::StringRef path,
                               const std::string& text,
                               const std::string& directory,
                               const std::vector<std::string>& arguments,
                               std::pair<std::string, uint32_t>& pch,
                               std::unordered_map<std::string, std::string>& pcms);

    /// Schedule background indexing when idle.
    void schedule_indexing();

    /// Background indexing coroutine.
    et::task<> run_background_indexing();

    /// Load PCH/PCM cache metadata from cache.json.
    void load_cache();

    /// Save PCH/PCM cache metadata to cache.json.
    void save_cache();

    /// Clean up stale cache files older than max_age_days.
    void cleanup_cache(int max_age_days = 7);

    // === Include/import completion (handled in master) ===

    PreambleCompletionContext detect_completion_context(const std::string& text, uint32_t offset);
    et::serde::RawValue complete_include(const PreambleCompletionContext& ctx,
                                         llvm::StringRef path);
    et::serde::RawValue complete_import(const PreambleCompletionContext& ctx);

    // === Feature request forwarding ===

    using RawResult = et::task<et::serde::RawValue, et::ipc::Error>;

    template <typename WorkerParams>
    RawResult forward_stateful(const std::string& uri);

    template <typename WorkerParams>
    RawResult forward_stateful(const std::string& uri, const protocol::Position& position);

    template <typename WorkerParams>
    RawResult forward_stateless(const std::string& uri, const protocol::Position& position);
};

}  // namespace clice
