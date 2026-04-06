#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "command/command.h"
#include "eventide/async/async.h"
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

struct DocumentState;

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

/// Manages compilation artifacts (PCH/PCM cache), compile arguments, module
/// dependency graph, and header context resolution.
///
/// `ensure_compiled()` remains in MasterServer because it deeply interacts
/// with document state, diagnostics publishing, and index updates.  Compiler
/// provides the lower-level building blocks it calls.
class Compiler {
public:
    Compiler(PathPool& path_pool,
             WorkerPool& pool,
             Indexer& indexer,
             const CliceConfig& config,
             CompilationDatabase& cdb,
             DependencyGraph& dep_graph);

    ~Compiler();

    // === Cache persistence ===

    void load_cache();
    void save_cache();
    void cleanup_cache(int max_age_days = 7);

    // === Module graph ===

    /// Build path_to_module reverse mapping from dependency graph.
    void build_module_map();

    /// Initialize the CompileGraph for C++20 module compilation ordering.
    void init_compile_graph();

    // === Compile argument resolution ===

    /// Fill compile arguments for a file (CDB lookup + header context fallback).
    bool fill_compile_args(llvm::StringRef path,
                           std::string& directory,
                           std::vector<std::string>& arguments);

    // === Dependency preparation ===

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

    // === Staleness detection ===

    /// Check if a file's AST or PCH deps have changed since last compile.
    bool is_stale(std::uint32_t path_id);

    /// Record dependency snapshot after a successful compile.
    void record_deps(std::uint32_t path_id, llvm::ArrayRef<std::string> deps);

    // === Lifecycle events (called from MasterServer handlers) ===

    /// Clean up state for a closed file.
    void on_file_closed(std::uint32_t path_id);

    /// Invalidate artifacts after a file save.
    /// Returns path_ids of all files dirtied (via compile_graph cascade).
    llvm::SmallVector<std::uint32_t> on_file_saved(std::uint32_t path_id);

    /// Invalidate header contexts whose host is the given file.
    /// Fills `stale_headers` with affected header path_ids.
    void invalidate_host_contexts(std::uint32_t host_path_id,
                                  llvm::SmallVectorImpl<std::uint32_t>& stale_headers);

    // === Header context ===

    /// Resolve a compilation context for a header that lacks a CDB entry.
    std::optional<HeaderFileContext> resolve_header_context(
        std::uint32_t header_path_id,
        const llvm::DenseMap<std::uint32_t, DocumentState>& documents);

    /// Set an active context override for a file.
    void switch_context(std::uint32_t path_id, std::uint32_t context_path_id);

    /// Get the active context override (if any).
    std::optional<std::uint32_t> get_active_context(std::uint32_t path_id) const;

    // === Access for MasterServer ===

    CompileGraph* compile_graph_ptr() { return compile_graph.get(); }
    const llvm::DenseMap<std::uint32_t, std::string>& module_map() const { return path_to_module; }

    /// Fill PCM paths for all built modules (for background indexing).
    void fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms) const;

    void cancel_all();

private:
    PathPool& path_pool;
    WorkerPool& pool;
    Indexer& indexer;
    const CliceConfig& config;
    CompilationDatabase& cdb;
    DependencyGraph& dep_graph;

    // PCH/PCM cache
    llvm::DenseMap<std::uint32_t, PCHState> pch_states;
    llvm::DenseMap<std::uint32_t, PCMState> pcm_states;
    llvm::DenseMap<std::uint32_t, std::string> pcm_paths;

    // Module info
    llvm::DenseMap<std::uint32_t, std::string> path_to_module;
    std::unique_ptr<CompileGraph> compile_graph;

    // Per-file compilation state
    llvm::DenseMap<std::uint32_t, DepsSnapshot> ast_deps;
    llvm::DenseMap<std::uint32_t, HeaderFileContext> header_file_contexts;
    llvm::DenseMap<std::uint32_t, std::uint32_t> active_contexts;

    // Internal helpers
    bool fill_header_context_args(llvm::StringRef path,
                                  std::uint32_t path_id,
                                  std::string& directory,
                                  std::vector<std::string>& arguments);
};

}  // namespace clice
