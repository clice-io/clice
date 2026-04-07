#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace et = eventide;
namespace protocol = et::ipc::protocol;

/// Context for compiling a header file that lacks its own CDB entry.
struct HeaderFileContext {
    std::uint32_t host_path_id;   /// Source file acting as host
    std::string preamble_path;    /// Path to generated preamble file on disk
    std::uint64_t preamble_hash;  /// Hash of preamble content for staleness
};

/// Two-layer staleness snapshot for compilation artifacts (PCH, AST, etc.).
///
/// Layer 1 (fast): compare each file's current mtime against build_at.
///   If all mtimes <= build_at, the artifact is fresh (zero I/O beyond stat).
///
/// Layer 2 (precise): for files whose mtime changed, re-hash their content
///   and compare against the stored hash.  If the hash matches, the file was
///   "touched" but not actually modified — skip the rebuild.
struct DepsSnapshot {
    llvm::SmallVector<std::uint32_t> path_ids;
    llvm::SmallVector<std::uint64_t> hashes;
    std::int64_t build_at = 0;
};

/// Cached PCH state for a single source file.
struct PCHState {
    std::string path;
    std::uint32_t bound = 0;
    std::uint64_t hash = 0;
    DepsSnapshot deps;
    std::shared_ptr<et::event> building;
};

/// Cached PCM state for a single module file.
struct PCMState {
    std::string path;
    DepsSnapshot deps;
};

/// State of a document opened by the client.
///
/// All per-open-file state lives here.  Open files are volatile and need
/// frequent updates, so their state (PCH, deps snapshot, header context)
/// must be independent from the stable, disk-based global state.
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

    /// Per-open-file PCH cache.  Only open files use PCH for incremental
    /// compilation; background indexing compiles from scratch.
    std::optional<PCHState> pch;

    /// Two-layer staleness snapshot captured after each successful compile.
    std::optional<DepsSnapshot> deps;

    /// Header context (only meaningful for header files without CDB entries).
    std::optional<HeaderFileContext> header_context;

    /// User-selected compilation context override (switchContext).
    std::optional<std::uint32_t> active_context;
};

enum class CompletionContext { None, IncludeQuoted, IncludeAngled, Import };

struct PreambleCompletionContext {
    CompletionContext kind = CompletionContext::None;
    std::string prefix;
};

/// Manages the full compilation lifecycle: document state, compilation
/// artifacts (PCH/PCM cache), compile argument resolution, header context,
/// and feature request forwarding to workers.
///
/// MasterServer delegates all document and compilation operations here,
/// keeping itself as a pure LSP handler registration layer.
class Compiler {
public:
    Compiler(et::event_loop& loop,
             et::ipc::JsonPeer& peer,
             PathPool& path_pool,
             WorkerPool& pool,
             Indexer& indexer,
             const CliceConfig& config,
             CompilationDatabase& cdb,
             DependencyGraph& dep_graph);

    ~Compiler();

    /// Convert a file:// URI to a local file path.
    static std::string uri_to_path(const std::string& uri);

    /// Document lifecycle — called from MasterServer handlers.
    void open_document(const std::string& uri, std::string text, int version);
    void apply_changes(const protocol::DidChangeTextDocumentParams& params);
    std::uint32_t close_document(const std::string& uri);
    llvm::SmallVector<std::uint32_t> on_save(const std::string& uri);

    /// Document accessors.
    bool is_file_open(std::uint32_t path_id) const;
    const DocumentState* get_document(std::uint32_t path_id) const;

    /// Cache persistence.
    void load_cache();
    void save_cache();
    void cleanup_cache(int max_age_days = 7);

    /// Build path_to_module reverse mapping from dependency graph.
    void build_module_map();

    /// Initialize the CompileGraph for C++20 module compilation ordering.
    void init_compile_graph();

    /// Fill compile arguments for a file (CDB lookup + header context fallback).
    bool fill_compile_args(llvm::StringRef path,
                           std::string& directory,
                           std::vector<std::string>& arguments);

    /// Fill PCM paths for all built modules (for background indexing).
    void fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                       std::uint32_t exclude_path_id = UINT32_MAX) const;

    /// Pull-based compilation entry point for user-opened files.
    et::task<bool> ensure_compiled(std::uint32_t path_id);

    /// Feature request forwarding to workers.
    using RawResult = et::task<et::serde::RawValue, et::ipc::Error>;

    /// Forward a stateful AST query to the worker owning this file.
    RawResult forward_query(worker::QueryKind kind, const std::string& uri);
    RawResult forward_query(worker::QueryKind kind,
                            const std::string& uri,
                            const protocol::Position& position);

    /// Forward a stateless build request (completion/signatureHelp).
    RawResult forward_build(worker::BuildKind kind,
                            const std::string& uri,
                            const protocol::Position& position);

    /// Completion with preamble-aware include/import handling.
    RawResult handle_completion(const std::string& uri, const protocol::Position& position);

    /// Header context management.
    void switch_context(std::uint32_t path_id, std::uint32_t context_path_id);
    std::optional<std::uint32_t> get_active_context(std::uint32_t path_id) const;
    void invalidate_host_contexts(std::uint32_t host_path_id,
                                  llvm::SmallVectorImpl<std::uint32_t>& stale_headers);

    CompileGraph* compile_graph_ptr() {
        return compile_graph.get();
    }

    const llvm::DenseMap<std::uint32_t, std::string>& module_map() const {
        return path_to_module;
    }

    void cancel_all();

    /// Callback invoked when indexing should be scheduled (e.g. after compile success).
    std::function<void()> on_indexing_needed;

private:
    /// Compile module dependencies, build/reuse PCH, and fill PCM paths.
    et::task<bool> ensure_deps(std::uint32_t path_id,
                               llvm::StringRef path,
                               const std::string& text,
                               const std::string& directory,
                               const std::vector<std::string>& arguments,
                               std::pair<std::string, uint32_t>& pch,
                               std::unordered_map<std::string, std::string>& pcms);

    /// Build or reuse PCH for a source file.
    et::task<bool> ensure_pch(std::uint32_t path_id,
                              llvm::StringRef path,
                              const std::string& text,
                              const std::string& directory,
                              const std::vector<std::string>& arguments);

    /// Check if a file's AST or PCH deps have changed since last compile.
    bool is_stale(std::uint32_t path_id);

    /// Record dependency snapshot after a successful compile.
    void record_deps(std::uint32_t path_id, llvm::ArrayRef<std::string> deps);

    void publish_diagnostics(const std::string& uri, int version, const et::serde::RawValue& diags);
    void clear_diagnostics(const std::string& uri);

    /// Clean up compilation state for a closed file.
    void on_file_closed(std::uint32_t path_id);

    /// Invalidate artifacts after a file save.
    /// Returns path_ids of all files dirtied (via compile_graph cascade).
    llvm::SmallVector<std::uint32_t> on_file_saved(std::uint32_t path_id);

    /// Header context resolution.
    std::optional<HeaderFileContext> resolve_header_context(std::uint32_t header_path_id);
    bool fill_header_context_args(llvm::StringRef path,
                                  std::uint32_t path_id,
                                  std::string& directory,
                                  std::vector<std::string>& arguments);

    /// Include/import completion helpers.
    PreambleCompletionContext detect_completion_context(const std::string& text, uint32_t offset);
    et::serde::RawValue complete_include(const PreambleCompletionContext& ctx,
                                         llvm::StringRef path);
    et::serde::RawValue complete_import(const PreambleCompletionContext& ctx);

private:
    et::event_loop& loop;
    et::ipc::JsonPeer& peer;
    PathPool& path_pool;
    WorkerPool& pool;
    Indexer& indexer;
    const CliceConfig& config;
    CompilationDatabase& cdb;
    DependencyGraph& dep_graph;

    /// Open document state, keyed by server-level path_id.
    llvm::DenseMap<std::uint32_t, DocumentState> documents;

    /// PCH warmup cache: loaded from cache.json on startup, moved into
    /// DocumentState when a file is opened.  This preserves PCH reuse
    /// across server restarts without polluting the global layer.
    llvm::DenseMap<std::uint32_t, PCHState> pch_warmup_cache;

    /// NOTE: A future "global PCH cache" for accelerating background indexing
    /// would live here as a separate map (e.g. DenseMap<uint32_t, PCHState>
    /// global_pch_cache).  Because PCH filenames are content-addressed
    /// ({xxh3_hash}.pch), per-open-file PCH and global PCH can coexist
    /// without path conflicts — their preamble content (buffer vs disk)
    /// naturally produces different hashes and different file paths.

    /// Global PCM cache state (shared across all files using that module).
    llvm::DenseMap<std::uint32_t, PCMState> pcm_states;
    llvm::DenseMap<std::uint32_t, std::string> pcm_paths;

    /// Module compilation ordering.
    llvm::DenseMap<std::uint32_t, std::string> path_to_module;
    std::unique_ptr<CompileGraph> compile_graph;
};

}  // namespace clice
