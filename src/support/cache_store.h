#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// Lifecycle policy for a cache namespace.
enum class CachePolicy : std::uint8_t {
    /// Size-capped with least-recently-used eviction: rebuildable large
    /// artifacts (PCH, PCM).
    LRU,

    /// Never evicted automatically, only via explicit invalidate():
    /// data that is expensive to accumulate (index).
    Persistent,

    /// Per-instance working files: not tracked in the manifest, not part
    /// of LRU.  Stored under a pid subdirectory; directories of dead pids
    /// are cleaned up when the namespace is registered.
    Scratch,
};

/// Static configuration of a cache namespace.
struct CacheNamespace {
    /// Directory name under the store root, e.g. "pch".
    std::string name;

    /// File extension appended to keys, including the dot, e.g. ".pch".
    std::string extension;

    CachePolicy policy = CachePolicy::LRU;

    /// Size budget for LRU namespaces; 0 means unlimited.
    /// Ignored for Persistent and Scratch.
    std::uint64_t max_bytes = 0;
};

/// Content-addressed blob store with atomic writes, crash recovery and
/// per-namespace lifecycle policies.
///
/// Responsibility split: the store only manages blob lifecycle — atomic
/// two-phase writes (begin_store/commit), LRU accounting and eviction,
/// orphan cleanup and the manifest checkpoint.  Keys are opaque,
/// filename-safe strings constructed by the caller (project convention:
/// hex of llvm::xxh3_128bits, optionally with a readable prefix).
/// Dependency tracking and staleness decisions are the caller's job.
///
/// On-disk layout under `{root}/cache/v{version}/`:
///   manifest.json        last-accessed checkpoint (not a source of truth)
///   tmp/{pid}/           in-flight writes of one live instance
///   {ns}/{key}{ext}      committed blobs (LRU / Persistent)
///   {ns}/{pid}/{key}{ext}  Scratch blobs of one live instance
///
/// A blob is complete iff it exists at its final path (atomic rename); the
/// only crash residue is tmp files, swept on open().  Opening a root whose
/// layout version differs discards the old directory entirely.
///
/// The store is passive: it owns no timer and never blocks waiting for IO
/// completion beyond the call itself.  Periodic checkpoint() scheduling is
/// the owner's responsibility.  All methods are thread-safe so that heavy
/// calls (commit's fsync, checkpoint) can be offloaded to a worker thread
/// while lookups continue on the event loop.
class CacheStore {
public:
    /// A two-phase write in progress.  The caller (or a worker process on
    /// its behalf) writes the blob to tmp_path, then either commits or
    /// aborts.  Dropping a PendingEntry without commit/abort leaks the tmp
    /// file until the next open() sweep.
    struct PendingEntry {
        std::string ns;
        std::string key;
        std::string tmp_path;
    };

    /// Open (creating if necessary) the store under `root`.  Any sibling
    /// version directory other than v{version} is deleted.  Loads the
    /// manifest if present and sweeps tmp directories of dead instances.
    static std::expected<CacheStore, std::error_code> open(llvm::StringRef root,
                                                           std::uint32_t version);

    CacheStore(CacheStore&&) noexcept;
    CacheStore& operator=(CacheStore&&) noexcept;
    ~CacheStore();

    /// Register a namespace and scan its directory to rebuild in-memory
    /// state.  Blobs already on disk are adopted; their last-accessed time
    /// comes from the manifest, falling back to file mtime.  For Scratch
    /// namespaces this cleans dead-pid subdirectories instead.
    void register_namespace(CacheNamespace ns);

    /// Return the absolute blob path on hit and refresh its in-memory
    /// last-accessed time (persisted on the next checkpoint).  No disk IO.
    std::optional<std::string> lookup(llvm::StringRef ns, llvm::StringRef key);

    /// Begin a two-phase write: returns a unique tmp path the blob must be
    /// written to (safe to hand to a worker process).
    PendingEntry begin_store(llvm::StringRef ns, llvm::StringRef key);

    /// Finish a two-phase write: fsync the tmp file and atomically rename
    /// it to its final path.  Triggers LRU eviction when the namespace
    /// exceeds its budget.  Returns the final blob path.
    std::expected<std::string, std::error_code> commit(PendingEntry pending);

    /// Cancel a two-phase write and delete the tmp file.
    void abort(const PendingEntry& pending);

    /// Remove a blob.  Primarily for Persistent namespaces, whose cleanup
    /// is the caller's mark-and-sweep; LRU namespaces rarely need it.
    void invalidate(llvm::StringRef ns, llvm::StringRef key);

    /// Enumerate all keys in a namespace (for caller-side mark-and-sweep).
    /// Iterates over a snapshot, so fn may call back into the store.
    void for_each_key(llvm::StringRef ns, llvm::function_ref<void(llvm::StringRef)> fn);

    /// The versioned root directory, e.g. `{root}/cache/v1`.  Callers may
    /// place their own metadata files directly under it (the store only
    /// manages namespace subdirectories); they die with the version.
    llvm::StringRef base_dir() const;

    /// Atomically persist the manifest (key sizes and last-accessed times)
    /// if anything changed.  Also runs automatically every few commits;
    /// the owner should additionally schedule it periodically and call
    /// shutdown() on exit.
    void checkpoint();

    /// Final checkpoint plus removal of this instance's tmp and Scratch
    /// directories.
    void shutdown();

private:
    struct State;

    explicit CacheStore(std::unique_ptr<State> state);

    /// Checkpoint if enough changes accumulated since the last one.
    void maybe_checkpoint();

    std::unique_ptr<State> state;
};

}  // namespace clice
