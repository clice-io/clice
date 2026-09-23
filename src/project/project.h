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
#include "index/writer_lock.h"
#include "project/build.h"
#include "project/hosting.h"
#include "semantic/symbol.h"
#include "support/cache_store.h"
#include "syntax/dependency_graph.h"
#include "vfs/file_table.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// On-disk cache layout version (CacheStore root `cache/v{N}`).
/// Bump to discard all cached artifacts after incompatible format changes.
constexpr inline std::uint32_t cache_format_version = 11;

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

    /// Path to the disk snapshot of the header itself, which the prefix
    /// includes in place of the header's other occurrences along the
    /// chain. Empty when the header could not be read.
    std::string snapshot_path;

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

/// The user's choice for a file in the editor (clice/switchContext): the
/// host to borrow a command from, or one of the file's own entries.
/// Persisted in the contexts blob; validated on didOpen.
struct Selection {
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

/// Cached PCH state.  Stored in Project.pch_cache keyed by the content
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
///   - Global layer (Project): tracks disk truth, shared by all files
///   - Per-file layer (Session): tracks buffer truth, isolated per TU
///
/// Project is the single source of truth for:
///   - dependency relationships (include graph, module DAG)
///   - compilation artifacts shared across files (PCH/PCM caches)
///   - symbol index (ProjectIndex + per-file Shard blobs)
///   - compilation database and configuration
///
/// Project is NEVER modified by unsaved buffer content.  The only mutation
/// paths are:
///   - Initialization  (load_root_project at startup)
///   - didSave         (rescan_after_save: rescan disk, cascade invalidation)
///   - Background index (merge TUIndex results from stateless workers)
struct Project {
    explicit Project(FileTable& file_table) : file_table(file_table) {}

    /// A default-constructed Config is born valid (every option holds its
    /// real default), so a directly-built Project (unit tests, tools)
    /// needs no init step. The server replaces this wholesale with the
    /// loaded user config and finalizes it after the initializationOptions
    /// overlay.
    Config config;

    /// The process's fid space, shared with everything else keyed by file
    /// (sessions, the task graph, the pool) — CDB entry file ids and
    /// project fids are the same ids. Persisted state never stores fids:
    /// it names files by path and re-interns them at load.
    FileTable& file_table;

    CompilationDatabase cdb{file_table};

    /// Which entries and hand-written commands apply to a file under the
    /// active configuration; the only reader of the rules.
    Build build{config, cdb, file_table};

    /// Unified on-disk blob store for PCH/PCM/index artifacts.  Opened by
    /// load_root_project() when cache_dir is configured; absent means caching
    /// is disabled.  Owns blob lifecycle (atomic writes, LRU, crash
    /// recovery); validity metadata (deps snapshots) lives in the index
    /// database, written by IndexStore::save.
    std::optional<CacheStore> store;

    /// The cache directory's writer lock, taken by a session that persists
    /// its index and held until the project dies — after `index_db`, so
    /// a reopened database never races another writer for the directory.
    std::optional<index::WriterLock> writer_lock;

    /// Index blob persistence, opened together with the cache store.
    /// Declared right after `store` (both backends borrow it) and before
    /// every index structure that borrows database bytes (`shards`), so
    /// destruction runs shards → index_db → store.
    std::unique_ptr<index::BlobDatabase> index_db;

    /// Include relationships between files on disk (#include edges).
    /// Built once at startup from CDB scan; updated incrementally on didSave.
    DependencyGraph dep_graph;

    /// PCH cache, keyed by content key (preamble text + canonical flags),
    /// so files with identical preambles share one PCH.  Hot-path mirror
    /// of CacheStore state; blob paths come from the store.
    llvm::StringMap<PCHState> pch_cache;

    /// PCM cache, keyed by module source path_id.
    llvm::DenseMap<Fid, PCMState> pcm_cache;

    /// The persisted index as loaded: the global symbol table, the per-TU
    /// manifests, the per-file row blobs and the name search index.
    index::ProjectIndex project_index;

    /// Monotonic generation of context-affecting project state (include
    /// graph, CDB, disk contents). Bumped on didSave; clice/queryContext
    /// stamps its results with it and clice/switchContext rejects requests
    /// made against an older epoch, so a client can never apply a context
    /// picked from a stale listing without noticing.
    std::uint64_t context_epoch = 1;

    /// Generation of the build's commands: bumped when a database reloads
    /// or a member appears, not on saves like context_epoch.
    std::uint64_t commands_epoch = 1;

    /// What a file without a command can borrow (see command_lender).
    LenderIndex lenders;

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

    /// Rescan a file after it was saved to disk, from one read: refresh
    /// its include edges (so host lookups and context queries see includes
    /// the save added or removed), its scanned hash and its module
    /// declaration. The module-graph cascade is the invalidator's job
    /// (PCMFamily::invalidate), and so is rebuilding the reverse include
    /// map — once per event batch, not once per rescanned file. Returns
    /// whether the file's include edges moved, which leaves the reverse
    /// map stale.
    bool rescan_after_save(Fid path_id);

    /// A file vanished from disk: it stops providing its module name (a
    /// replacement provider would otherwise sit behind it and never be
    /// selected) and its import syntax (the last import-bearing file must
    /// release the project-wide scan gate), and its outgoing edges go, so
    /// it stops being a host candidate. Incoming edges stay — includers'
    /// text still names it, and their own rescans own those edges. The
    /// reverse include map is left to the caller's batch, as for rescans.
    void forget_file(Fid path_id);

    /// What rebuilding the dependency graph did to module providers, per
    /// name: the provider import resolution selects (the candidate list's
    /// head), not mere existence.
    struct ProviderChanges {
        /// Names that gained their first provider.
        llvm::SmallVector<std::string> appeared;

        /// The previously selected providers of names whose selection moved
        /// to another file.
        llvm::SmallVector<Fid> replaced;
    };

    /// Rebuild the dependency graph from scratch against the current
    /// database: entry additions, removals and flag changes all funnel into
    /// one uniform rescan instead of per-entry graph surgery. Still cheap —
    /// per-file scan results are content-keyed in the file table, so
    /// unchanged files re-resolve without a read or lex.
    ProviderChanges rebuild_dependency_graph();

    /// Persistence signal for the artifact validity metadata (PCH/PCM
    /// records, header modes) the index database carries beyond the index
    /// itself: producers mark, the single write pipeline (IndexStore::save)
    /// flushes it on its next run.
    bool artifacts_dirty = false;

    /// Wired by the master to schedule a flush soon after a mark; unset
    /// (tests, batch tools) means the owner saves on its own cadence.
    std::function<void()> request_flush;

    void mark_artifacts_dirty() {
        artifacts_dirty = true;
        if(request_flush) {
            request_flush();
        }
    }

    /// Fill PCM paths for all built modules, excluding exclude_path_id.
    void fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                       Fid exclude_path_id = {}) const;
};

/// The `compile_commands.json` files to load when no rule declares one:
/// the workspace root's, then those of its direct subdirectories in name
/// order. Empty when none exists yet — the file tracker keeps looking on
/// its CDB poll.
llvm::SmallVector<std::string> discover_compile_commands(llvm::StringRef workspace_root);

/// Every `compile_commands.json` under `workspace_root` (`.git` and the
/// cache directory skipped): what the one-shot batch commands, which
/// open no file, discover instead of waiting for a didOpen.
llvm::SmallVector<std::string> compile_commands_below(llvm::StringRef workspace_root,
                                                      llvm::StringRef cache_dir);

/// The `compile_commands.json` files in `start` and its ancestors up to
/// `workspace_root`, nearest first: the databases a file deeper in the
/// tree than startup discovery looks may compile from.
llvm::SmallVector<std::string> compile_commands_above(llvm::StringRef start,
                                                      llvm::StringRef workspace_root);

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
