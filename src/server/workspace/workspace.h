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
#include "index/merged_index.h"
#include "index/project_index.h"
#include "semantic/relation_kind.h"
#include "server/compiler/compile_graph.h"
#include "server/workspace/config.h"
#include "support/cache_store.h"
#include "support/path_pool.h"
#include "syntax/dependency_graph.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// On-disk cache layout version (CacheStore root `cache/v{N}`).
/// Bump to discard all cached artifacts after incompatible format changes.
constexpr inline std::uint32_t cache_format_version = 1;

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

/// Context for compiling a header file that lacks its own CDB entry.
struct HeaderFileContext {
    std::uint32_t host_path_id;   ///< Source file acting as host.
    std::string preamble_path;    ///< Path to generated preamble file on disk.
    std::uint64_t preamble_hash;  ///< Hash of preamble content for staleness.
};

/// Cached PCH state.  Content-addressed by preamble text + frontend compile
/// flags — shared across all files (open or on-disk) with the same key.
struct PCHState {
    std::string path;
    std::uint32_t bound = 0;
    /// CacheStore key: hex of xxh3_128bits(preamble text + canonical flags).
    std::string key;
    DepsSnapshot deps;
    std::string document_links_json;  ///< Pre-serialized DocumentLink[] from PCH build
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
///   - didSave         (on_file_saved: rescan disk, cascade invalidation)
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

    /// PCH cache, keyed by file path_id.  Hot-path mirror of CacheStore
    /// state; blob paths come from the store.
    /// TODO: re-key by content hash to enable cross-file sharing.
    llvm::DenseMap<std::uint32_t, PCHState> pch_cache;

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

    /// Called when a file is saved to disk.  Cascades invalidation through
    /// compile_graph and clears affected PCM caches.
    /// Returns path_ids of all files dirtied by the cascade.
    llvm::SmallVector<std::uint32_t> on_file_saved(std::uint32_t path_id);

    /// Called when a file is closed.  Notifies compile_graph if this file
    /// is a module unit so dependents can be re-evaluated on next compile.
    void on_file_closed(std::uint32_t path_id);

    /// Load PCH/PCM validity metadata from cache.json (under the store's
    /// versioned root); entries whose blob is gone from the store are dropped.
    void load_cache();
    /// Save PCH/PCM validity metadata to cache.json.
    void save_cache();
    /// Build path_to_module reverse mapping from dep_graph.
    void build_module_map();
    /// Fill PCM paths for all built modules, excluding exclude_path_id.
    void fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                       std::uint32_t exclude_path_id = UINT32_MAX) const;
    /// Cancel all in-flight compilations.
    void cancel_all();
};

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
