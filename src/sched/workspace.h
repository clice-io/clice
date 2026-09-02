#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "command/command.h"
#include "compile/dep_file.h"
#include "config/config.h"
#include "index/database.h"
#include "index/project_index.h"
#include "index/shard.h"
#include "index/tu_index.h"
#include "sched/crash_budget.h"
#include "semantic/symbol.h"
#include "support/cache_store.h"
#include "syntax/dependency_graph.h"
#include "vfs/file_table.h"

#include "kota/async/async.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class ContextResolver;

/// On-disk cache layout version (CacheStore root `cache/v{N}`).
/// Bump to discard all cached artifacts after incompatible format changes.
constexpr inline std::uint32_t cache_format_version = 8;

/// One dependency of a compilation artifact.
///
/// `version` names the FileVersion the build actually consumed (interned
/// from the worker-reported content hash) — the stat fast path and the
/// two-layer freshness test live on the shared version, paid once per
/// wave for every artifact and TU referencing it (FileTable::
/// check_version). An invalid version means the build saw no nameable
/// bytes: `missing` distinguishes "the file was absent" (reappearing is
/// the change) from "the bytes could not be hashed" (stale until a
/// rebuild's capture converges).
struct DepState {
    Fid path_id;
    VersionID version;
    bool missing = false;
};

/// Staleness snapshot for compilation artifacts (PCH, PCM, AST, synthesized
/// header preambles): the consumed versions, checked via deps_changed.
using DepsSnapshot = llvm::SmallVector<DepState>;

/// Drop every trust anchor of the snapshot's files so the next check
/// re-validates each dependency by a real read (the versions stay: they
/// describe what the artifact was built from). Used when embedded copies
/// of dependency content may disagree with the files themselves.
/// Shared-level on purpose: the anchors live on the versions, so other
/// consumers of a forced file pay one re-read too.
void force_revalidate_deps(FileTable& files, const DepsSnapshot& snap);

/// Context for compiling a header file that lacks its own CDB entry.
/// The cache-store namespace of synthesized header-context files
/// (preamble, suffix and self snapshot): content-addressed blobs, so a
/// header reopened in a later session finds its preamble — and the PCH
/// keyed on the preamble's path — intact.
constexpr inline llvm::StringLiteral header_context_ns = "header_context";

struct HeaderContext {
    Fid host_path_id;             ///< Source file acting as host.
    std::string preamble_path;    ///< Path to generated preamble file on disk.
    std::uint64_t preamble_hash;  ///< Hash of preamble content for staleness.

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

    /// Base entry hash of that entry (before rules): stays unique when
    /// rules collapse two candidates' applied hashes onto one value.
    std::string host_base_hash;

    /// Include chain from host to the target's direct includer (excludes the
    /// target itself). The synthesized preamble embeds these files' content,
    /// so clang never opens them — staleness must be tracked here.
    llvm::SmallVector<Fid> chain;

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
    /// Header context host; invalid = none.
    Fid host_path_id;

    /// Pinned include occurrence; no value = automatic.
    std::optional<std::uint32_t> occurrence;

    std::string command_hash;  ///< Pinned CDB entry (rules applied); empty = none.

    /// Base entry hash of the pinned entry, resolved at pin time. The
    /// applied hash is the protocol identity; the base disambiguates
    /// candidates whose applied hashes collapse under the current rules.
    std::string base_hash;
};

/// Cached PCH state.  Stored in Workspace.pch_cache keyed by the content
/// key (hex of xxh3_128bits over preamble text + directories + canonical
/// flags), so files with identical preambles share one PCH.
///
/// Everything derived from the PCH build beyond validity metadata — the
/// preamble's symbol index, document links, inactive regions, the open
/// conditional stack — lives in the paired pch.idx envelope (the store's
/// `.pch.idx` aux file), committed and evicted together with the PCH.
/// Open a PCH's `.pch.idx` envelope (memory-mapped). Returns nullptr when
/// the file is unreadable, structurally invalid, of a different format
/// version, or any embedded shard blob fails verification — callers treat
/// all of these as a PCH cache miss.
std::shared_ptr<index::TUIndex> load_pch_envelope(llvm::StringRef path);

struct PCHState {
    std::string path;
    std::uint32_t bound = 0;
    DepsSnapshot deps;

    /// Path of the paired pch.idx envelope.
    std::string index_path;

    /// Lazily opened blob; shared so a consumer holding it across an await
    /// survives concurrent entry replacement or eviction.
    std::shared_ptr<index::TUIndex> state;

    /// Open the blob on first use (memory-mapped, no deserialization).
    /// Returns nullptr when the blob is missing or unreadable — consumers
    /// degrade (no overlay, no preamble links) and the next PCH round
    /// treats the incomplete pair as a cache miss.
    const std::shared_ptr<index::TUIndex>& load_state();
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
///   - symbol index (ProjectIndex + per-file Shard blobs)
///   - compilation database and configuration
///
/// Workspace is NEVER modified by unsaved buffer content.  The only mutation
/// paths are:
///   - Initialization  (load_workspace at startup)
///   - didSave         (rescan_after_save: rescan disk, cascade invalidation)
///   - Background index (merge TUIndex results from stateless workers)
struct Workspace {
    /// A default-constructed Config is born valid (every option holds its
    /// real default), so a directly-built Workspace (unit tests, tools)
    /// needs no init step. The server replaces this wholesale with the
    /// loaded user config and finalizes it after the initializationOptions
    /// overlay.
    Config config;

    /// The single fid space, shared by everything below — CDB entry file
    /// ids and workspace fids are the same ids.
    FileTable file_table;

    CompilationDatabase cdb{file_table};

    /// Unified on-disk blob store for PCH/PCM/index artifacts.  Opened by
    /// load_workspace() when cache_dir is configured; absent means caching
    /// is disabled.  Owns blob lifecycle (atomic writes, LRU, crash
    /// recovery); validity metadata (deps snapshots) lives in the index
    /// database, written by IndexStore::save.
    std::optional<CacheStore> store;

    /// Index blob persistence, opened together with the cache store.
    /// Declared right after `store` (both backends borrow it) and before
    /// every index structure that borrows database bytes (`shards`), so
    /// destruction runs shards → index_db → store.
    std::unique_ptr<index::BlobDatabase> index_db;

    /// Include relationships between files on disk (#include edges).
    /// Built once at startup from CDB scan; updated incrementally on didSave.
    DependencyGraph dep_graph;

    /// Reverse mapping: file path_id → module name (e.g. "std", "foo.bar").
    /// Built from dep_graph at startup; updated on didSave when module
    /// declarations change.
    llvm::DenseMap<Fid, std::string> path_to_module;

    /// PCH cache, keyed by content key (preamble text + canonical flags),
    /// so files with identical preambles share one PCH.  Hot-path mirror
    /// of CacheStore state; blob paths come from the store.
    llvm::StringMap<PCHState> pch_cache;

    /// Keys of pch_cache entries whose envelope is currently loaded,
    /// most recently used first (see enforce_loaded_budget).
    llvm::SmallVector<std::string, 8> loaded_state_lru;

    /// Open-document count provider, wired by the master. Sizes the
    /// loaded-state budget; unset (tests, tools) falls back to
    /// default_open_documents.
    std::function<std::size_t()> open_documents;

    /// Crash budget for shared build artifacts (PCH/PCM), keyed by the
    /// same content-derived cache keys: an artifact that keeps killing
    /// workers is refused until its content — and therefore its key —
    /// changes. Document quarantine cannot contain these: the artifact is
    /// shared, so every dependent would burn workers of its own.
    CrashBudget build_crashes;

    /// PCM cache, keyed by module source path_id.
    llvm::DenseMap<Fid, PCMState> pcm_cache;

    /// The index's global layer: symbols, FileVersions, per-TU manifests
    /// and the derived contribution map.
    index::ProjectIndex project_index;

    /// Per-file row blobs from background indexing, keyed by project-level
    /// path_id: symbol occurrences, relations and stored content for
    /// position mapping, served zero-copy.
    llvm::DenseMap<Fid, index::Shard> shards;

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
    std::uint32_t count_occurrences(Fid host_id, Fid target_id) const;

    /// Rank host source candidates for a header by relevance: a source
    /// with the header's stem (utils.h -> utils.cpp) wins, then sources in
    /// the same directory, then longer common path prefixes; ties break
    /// lexicographically so the choice is deterministic.
    llvm::SmallVector<Fid> rank_hosts(Fid header_path_id, llvm::ArrayRef<Fid> hosts) const;

    /// Rescan a file after it was saved to disk, from one read: refresh
    /// its include edges (so host lookups and context queries see includes
    /// the save added or removed) and its module declaration. The
    /// module-graph cascade is the invalidator's job
    /// (PCMFamily::invalidate).
    void rescan_after_save(Fid path_id);

    /// Called when a file is closed.  Notifies compile_graph if this file
    /// is a module unit so dependents can be re-evaluated on next compile.
    void on_file_closed(Fid path_id);

    /// Open the pch.idx envelope of a cached PCH. The single consumption
    /// gate for `.pch.idx` blobs: when the blob turns out unreadable, the
    /// on-disk pair is retracted from the store as well — otherwise every
    /// later session re-adopts the corrupt pair from the artifacts blob and
    /// silently degrades again. With the pair gone the next ensure_pch is
    /// a miss and rebuilds both halves. Loads count against the
    /// loaded-state budget (see enforce_loaded_budget).
    std::shared_ptr<index::TUIndex> preamble_state(llvm::StringRef pch_key);

    /// Move a pch key to the front of the loaded-state LRU. Called
    /// whenever an entry's envelope is opened or replaced.
    void touch_loaded_state(llvm::StringRef pch_key);

    /// Unload pch.idx envelopes beyond the budget (open documents + 2),
    /// least recently used first. Without this every preamble key ever
    /// touched keeps its blob mapped for the server's lifetime — tens of
    /// MB per key on real projects, released by neither didClose nor
    /// store eviction. Unloading only drops the entry's reference:
    /// consumers holding the shared_ptr finish safely, and the next use
    /// reopens the blob from disk.
    void enforce_loaded_budget();

    /// Persistence signals for the metadata the index database carries
    /// beyond the index itself: artifact validity (PCH/PCM records, header
    /// modes) and user context choices. Producers mark; the single write
    /// pipeline (IndexStore::save) flushes both on its next run. Only the
    /// contexts blob has a durability waiter (switchContext), so only it
    /// carries an epoch: the ticket resolves once a save whose snapshot
    /// covers the mark commits that blob — independent of the artifacts
    /// blob, whose failures retry through the dirty flag alone and must
    /// not hold a context ack hostage. `contexts_committed` pulses after
    /// every attempt, failed ones included, so waiters can give up on a
    /// disk that cannot take the write.
    bool artifacts_dirty = false;
    bool contexts_dirty = false;
    std::uint64_t contexts_epoch = 0;
    std::uint64_t committed_contexts_epoch = 0;
    kota::event contexts_committed;

    /// Wired by the master to schedule a flush soon after a mark; unset
    /// (tests, batch tools) means the owner saves on its own cadence.
    std::function<void()> request_flush;

    void mark_artifacts_dirty() {
        artifacts_dirty = true;
        if(request_flush) {
            request_flush();
        }
    }

    void mark_contexts_dirty() {
        contexts_dirty = true;
        contexts_epoch += 1;
        if(request_flush) {
            request_flush();
        }
    }

    /// Build path_to_module reverse mapping from dep_graph.
    void build_module_map();
    /// Fill PCM paths for all built modules, excluding exclude_path_id.
    void fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                       Fid exclude_path_id = {}) const;
};

/// Find the workspace's compile_commands.json: the configured paths first
/// (a directory means <dir>/compile_commands.json), then the workspace root,
/// then its direct subdirectories. Returns the empty string when none
/// exists yet — the file tracker keeps looking on its CDB poll.
std::string discover_compile_commands(const Config& config, llvm::StringRef workspace_root);

/// Capture a staleness snapshot from a build's reported inputs, interning
/// the consumed versions into the shared table.
///
/// `deps` carries the consumed-content hashes the worker computed at build
/// time; `build_at` is milliseconds since epoch, sampled before the build
/// started. Each dependency is stat'ed once: a file untouched since
/// `build_at` offers its stat as the version's fast-path baseline
/// (recorded only when corroborated, see FileTable::try_stamp), a file
/// modified during or after the build offers none — the next check must
/// prove the disk still matches the consumed hash before trusting (and
/// repairing) the stat.
DepsSnapshot capture_deps_snapshot(FileTable& files,
                                   llvm::ArrayRef<DepFile> deps,
                                   std::int64_t build_at);

/// Whether any consumed version stopped matching the disk; see
/// FileTable::check_version for the two-layer test and DepState for the
/// per-reference missing policy. Callers open the memo wave.
bool deps_changed(FileTable& files, const DepsSnapshot& snap);

}  // namespace clice
