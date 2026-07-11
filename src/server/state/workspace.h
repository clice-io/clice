#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "command/command.h"
#include "command/toolchain.h"
#include "feature/document_link.h"
#include "index/merged_index.h"
#include "index/preamble_state.h"
#include "index/project_index.h"
#include "semantic/relation_kind.h"
#include "server/compiler/compile_graph.h"
#include "server/state/config.h"
#include "support/cache_store.h"
#include "support/path_pool.h"
#include "syntax/dependency_graph.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class ContextResolver;

/// On-disk cache layout version (CacheStore root `cache/v{N}`).
/// Bump to discard all cached artifacts after incompatible format changes.
constexpr inline std::uint32_t cache_format_version = 4;

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

/// Sentinel for "no path": path pool ids start at 0, so 0 is a real file.
constexpr inline std::uint32_t no_path_id = ~0u;

/// Context for compiling a header file that lacks its own CDB entry.
struct HeaderContext {
    std::uint32_t host_path_id = no_path_id;  ///< Source file acting as host.
    std::string preamble_path;                ///< Path to generated preamble file on disk.
    std::uint64_t preamble_hash;              ///< Hash of preamble content for staleness.

    /// Path to the generated suffix file (content after the include
    /// position along the chain), appended to the header's buffer as one
    /// trailing #include line. Empty when the suffix is empty.
    std::string suffix_path;

    /// Which include of this header in its direct includer produced the
    /// preamble (0-based, in directive order).
    std::uint32_t occurrence = 0;

    /// Canonical hash of the host CDB entry used (multi-configuration
    /// hosts); empty = the first entry.
    std::string host_command_hash;

    /// Include chain from host to the target's direct includer (excludes the
    /// target itself). The synthesized preamble embeds these files' content,
    /// so clang never opens them — staleness must be tracked here.
    llvm::SmallVector<std::uint32_t> chain;

    /// Staleness snapshot over the chain files (mtime + content hash).
    DepsSnapshot deps;
};

/// Whether a header can compile on its own (given a borrowed command)
/// or needs a synthesized prefix restoring the includer's preprocessor
/// state. Determined by compiling self-contained first and falling back
/// when the diagnostics indicate missing context.
enum class HeaderMode : std::uint32_t {
    Unknown = 0,
    SelfContained = 1,
    NeedsContext = 2,
};

/// A user's context choice, persisted across sessions.
struct SavedContext {
    /// Header context host; no_path_id = none.
    std::uint32_t host_path_id = no_path_id;

    /// Pinned include occurrence; no value = automatic.
    std::optional<std::uint32_t> occurrence;

    std::string command_hash;  ///< Pinned CDB entry; empty = none.
};

/// Cached PCH state.  Stored in Workspace.pch_cache keyed by the content
/// key (hex of xxh3_128bits over preamble text + directories + canonical
/// flags), so files with identical preambles share one PCH.
///
/// Everything derived from the PCH build beyond validity metadata — the
/// preamble's symbol index, document links, inactive regions, the open
/// conditional stack — lives in the paired PreambleState blob (the store's
/// `.pch.idx` aux file), committed and evicted together with the PCH.
struct PCHState {
    std::string path;
    std::uint32_t bound = 0;
    DepsSnapshot deps;

    /// Path of the paired PreambleState blob.
    std::string index_path;

    /// Lazily opened blob; shared so a consumer holding it across an await
    /// survives concurrent entry replacement or eviction.
    std::shared_ptr<index::PreambleState> state;

    /// Open the blob on first use (memory-mapped, no deserialization).
    /// Returns nullptr when the blob is missing or unreadable — consumers
    /// degrade (no overlay, no preamble links) and the next ensure_pch
    /// treats the incomplete pair as a cache miss.
    const std::shared_ptr<index::PreambleState>& load_state();

    std::shared_ptr<kota::event> building;
};

/// Cached PCM state for a single C++20 module.  Shared across all files that
/// import the same module.
struct PCMState {
    std::string path;
    /// CacheStore key: "{module}-{hash}" over source path + canonical flags.
    std::string key;
    DepsSnapshot deps;
};

/// All persistent, project-wide state derived from files on disk.
///
/// Design principle: open files are never depended upon by other files.
/// Dependencies always point to disk files.  This enforces a clean two-layer
/// architecture:
///   - Global layer (Workspace): tracks disk truth, shared by all files
///   - Per-file layer (Session): tracks buffer truth, isolated per TU
///
/// Workspace is the single source of truth for:
///   - dependency relationships (include graph, module DAG)
///   - compilation artifacts shared across files (PCH/PCM caches)
///   - symbol index (ProjectIndex + per-file MergedIndex shards)
///   - compilation database and configuration
///
/// Workspace is NEVER modified by unsaved buffer content.  The only mutation
/// paths are:
///   - Initialization  (load_workspace at startup)
///   - didSave         (rescan_after_save: rescan disk, cascade invalidation)
///   - Background index (merge TUIndex results from stateless workers)
struct Workspace {
    Config config;
    CompilationDatabase cdb;
    Toolchain toolchain;

    PathPool path_pool;

    /// Unified on-disk blob store for PCH/PCM/index artifacts.  Opened by
    /// load_workspace() when cache_dir is configured; absent means caching
    /// is disabled.  Owns blob lifecycle (atomic writes, LRU, crash
    /// recovery); validity metadata (deps snapshots) stays in cache.json.
    std::optional<CacheStore> store;

    /// Include relationships between files on disk (#include edges).
    /// Built once at startup from CDB scan; updated incrementally on didSave.
    DependencyGraph dep_graph;

    /// C++20 module compilation ordering DAG.
    /// Lazily resolves module dependencies; updated on didSave via cascade.
    std::unique_ptr<CompileGraph> compile_graph;

    /// Reverse mapping: file path_id → module name (e.g. "std", "foo.bar").
    /// Built from dep_graph at startup; updated on didSave when module
    /// declarations change.
    llvm::DenseMap<std::uint32_t, std::string> path_to_module;

    /// PCH cache, keyed by content key (preamble text + canonical flags),
    /// so files with identical preambles share one PCH.  Hot-path mirror
    /// of CacheStore state; blob paths come from the store.
    llvm::StringMap<PCHState> pch_cache;

    /// PCM cache, keyed by module source path_id.
    llvm::DenseMap<std::uint32_t, PCMState> pcm_cache;

    /// PCM output paths, keyed by module source path_id.
    /// Maps to the .pcm file on disk used as -fmodule-file argument.
    llvm::DenseMap<std::uint32_t, std::string> pcm_paths;

    /// Global symbol table across all indexed translation units.
    index::ProjectIndex project_index;

    /// Per-file index shards from background indexing, keyed by project-level
    /// path_id.  Contains symbol occurrences, relations, and stored content
    /// for position mapping.
    llvm::DenseMap<std::uint32_t, index::MergedIndex> merged_indices;

    /// Monotonic generation of context-affecting workspace state (include
    /// graph, CDB, disk contents). Bumped on didSave; clice/queryContext
    /// stamps its results with it and clice/switchContext rejects requests
    /// made against an older epoch, so a client can never apply a context
    /// picked from a stale listing without noticing.
    std::uint64_t context_epoch = 1;

    /// Whether `path` is one of our own synthesized context artifacts
    /// (prefix/suffix/self-snapshot files under the cache directory). A
    /// user can open these for debugging; they must never go through
    /// header-context resolution themselves — a synthesized file deriving
    /// context from other synthesized files would chain junk state.
    bool is_synthesized_artifact(llvm::StringRef path) const;

    /// How many times the direct includer on host->target's chain includes
    /// the target. Spelling-based (no search-path resolution): multiple
    /// inclusions of one header always share a spelling, and synthesis
    /// validates the real occurrence anyway.
    std::uint32_t count_occurrences(std::uint32_t host_id, std::uint32_t target_id) const;

    /// Rank host source candidates for a header by relevance: a source
    /// with the header's stem (utils.h -> utils.cpp) wins, then sources in
    /// the same directory, then longer common path prefixes; ties break
    /// lexicographically so the choice is deterministic.
    llvm::SmallVector<std::uint32_t> rank_hosts(std::uint32_t header_path_id,
                                                llvm::ArrayRef<std::uint32_t> hosts) const;

    /// Re-resolve a saved file's direct include edges from its disk
    /// content, so host lookups and context queries see includes the save
    /// added or removed.
    void rescan_includes(std::uint32_t path_id);

    /// Rescan a file after it was saved to disk.  Cascades invalidation
    /// through compile_graph and clears affected PCM caches.
    /// Returns path_ids of all files dirtied by the cascade.
    llvm::SmallVector<std::uint32_t> rescan_after_save(std::uint32_t path_id);

    /// Called when a file is closed.  Notifies compile_graph if this file
    /// is a module unit so dependents can be re-evaluated on next compile.
    void on_file_closed(std::uint32_t path_id);

    /// Load PCH/PCM validity metadata plus the context resolver's slices
    /// from cache.json (under the store's versioned root); entries whose
    /// blob is gone from the store are dropped.
    void load_cache(ContextResolver& contexts);
    /// Save PCH/PCM validity metadata plus the context resolver's slices
    /// to cache.json.
    void save_cache(const ContextResolver& contexts);
    /// Build path_to_module reverse mapping from dep_graph.
    void build_module_map();
    /// Fill PCM paths for all built modules, excluding exclude_path_id.
    void fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                       std::uint32_t exclude_path_id = UINT32_MAX) const;
    /// Cancel all in-flight compilations.
    void cancel_all();
};

/// Find the workspace's compile_commands.json: the configured paths first
/// (a directory means <dir>/compile_commands.json), then the workspace root,
/// then its direct subdirectories. Returns the empty string when none
/// exists yet — the file tracker keeps looking on its CDB poll.
std::string discover_compile_commands(const Config& config, llvm::StringRef workspace_root);

/// Hash a file's content using xxh3_64bits. Returns 0 on read failure.
std::uint64_t hash_file(llvm::StringRef path);

/// Capture a two-layer staleness snapshot after a successful compilation.
/// Interns dependency paths into the PathPool and hashes each file's content.
DepsSnapshot capture_deps_snapshot(PathPool& pool, llvm::ArrayRef<std::string> deps);

/// Two-layer staleness check.
/// Layer 1 (fast): stat each dep file, compare mtime against build_at.
/// Layer 2 (precise): for files with mtime > build_at, re-hash content.
bool deps_changed(const PathPool& pool, const DepsSnapshot& snap);

}  // namespace clice
