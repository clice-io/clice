#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

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

    /// Optional paired-blob extension, e.g. ".pch.idx".  When set, a key
    /// owns two files — `{key}{extension}` plus `{key}{aux_extension}` —
    /// forming a single entry: sized, aged and evicted together.  The
    /// primary is committed first; committing it resets any stale aux
    /// blob, so a pair is only served complete (see lookup_aux).  Must not
    /// be a suffix collision with `extension`.
    std::string aux_extension;

    CachePolicy policy = CachePolicy::LRU;

    /// Size budget for LRU namespaces; 0 means unlimited.
    /// Ignored for Persistent and Scratch.
    std::uint64_t max_bytes = 0;

    /// The blobs' validity metadata lives outside this store (the index
    /// database's artifacts blob) and is flushed after the fact: commits
    /// here raise the writer-dirty marker until that metadata is durable.
    /// Self-describing namespaces (the index database's own files) leave
    /// it unset, or the marker could never clear while they keep writing.
    bool deferred_metadata = false;
};

/// The identity of one committed blob, captured while the bytes were still
/// private (the tmp file, before the publishing rename — capturing from
/// the final path would race a concurrent replacer of the same key). The
/// rename preserves inode and mtime, so the values stay true afterwards:
/// the stat triple is a cheap per-use freshness check, and — since every
/// commit is a fresh tmp file — the UniqueID changes whenever the blob is
/// republished, making it a free generation token that same-size same-tick
/// rewrites cannot forge. `hash` (xxh3 of the bytes) is the deep anchor
/// for whole-read consumers and post-crash verification.
struct BlobBinding {
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::uint64_t uid_device = 0;
    std::uint64_t uid_file = 0;
    std::uint64_t hash = 0;
};

/// Whether the file at `path` still carries the binding's stat identity
/// (size, mtime, UniqueID) — the cheap per-use check that the blob was not
/// republished since the record was made. A zeroed binding (record from
/// before the blob existed) never matches.
bool binding_stat_matches(llvm::StringRef path, const BlobBinding& binding);

/// Whether the file's bytes hash to the binding's. Reads the whole file;
/// used for post-crash verification and whole-read consumers.
bool binding_content_matches(llvm::StringRef path, const BlobBinding& binding);

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
/// while lookups continue on the event loop.  A synchronous operation runs
/// to completion once started, so cancellation can never observe a torn
/// mid-operation state.
///
/// TODO: once usage settles, evaluate making commit/checkpoint coroutines
/// over kota's async fs (kota::fsync/rename) with all state confined to
/// the event loop — that removes the mutex entirely, but moves the burden
/// from lock discipline to cancellation points inside each operation.
class CacheStore {
public:
    /// A two-phase write in progress.  The caller (or a worker process on
    /// its behalf) writes the blob to tmp_path, then commits.  Self-cleaning:
    /// an entry destroyed without being committed removes its tmp file —
    /// kotatsu cancellation destroys a suspended coroutine frame without
    /// resuming it, so a manual abort after the await would never run on
    /// that path and the tmp blob would leak until the next open() sweep.
    struct PendingEntry {
        std::string ns;
        std::string key;
        std::string tmp_path;

        /// Whether this write targets the key's aux blob (begin_store_aux).
        bool aux = false;

        PendingEntry() = default;

        PendingEntry(std::string ns, std::string key, std::string tmp_path, bool aux = false) :
            ns(std::move(ns)), key(std::move(key)), tmp_path(std::move(tmp_path)), aux(aux) {}

        PendingEntry(const PendingEntry&) = delete;
        PendingEntry& operator=(const PendingEntry&) = delete;

        PendingEntry(PendingEntry&& other) noexcept :
            ns(std::move(other.ns)), key(std::move(other.key)), tmp_path(std::move(other.tmp_path)),
            aux(other.aux) {
            other.tmp_path.clear();
        }

        PendingEntry& operator=(PendingEntry&& other) noexcept {
            if(this != &other) {
                remove_tmp();
                ns = std::move(other.ns);
                key = std::move(other.key);
                tmp_path = std::move(other.tmp_path);
                aux = other.aux;
                other.tmp_path.clear();
            }
            return *this;
        }

        ~PendingEntry() {
            remove_tmp();
        }

    private:
        /// Idempotent: commit() consumes the entry by value, and after its
        /// rename the tmp path no longer exists — removing a missing file
        /// is a no-op.
        void remove_tmp();
    };

    /// Open (creating if necessary) the store under `root`.  Any sibling
    /// version directory other than v{version} is deleted.  Loads the
    /// manifest if present and sweeps tmp directories of dead instances.
    ///
    /// `read_only` opens for inspection without touching the disk: nothing
    /// is created, swept or discarded — a store another process (possibly
    /// an older layout version) is live on must survive being inspected.
    /// Fails with `no_such_file_or_directory` when the versioned directory
    /// does not exist; only lookups and enumeration may be used.
    /// `adopt_writer_debt` says this session can discharge a dead writer's
    /// verification debt (it persists artifact metadata). A session that
    /// cannot (read-only index database) still verifies adopted records
    /// itself but hands the marker on at shutdown instead of clearing it.
    static std::expected<CacheStore, std::error_code> open(llvm::StringRef root,
                                                           std::uint32_t version,
                                                           bool read_only = false,
                                                           bool adopt_writer_debt = true);

    /// Drop self-ignore markers into `root` (created if missing): a
    /// `.gitignore` and a CACHEDIR.TAG. Sessions call this right after
    /// resolving the configuration, before anything (session logs, the
    /// store itself) lands content under the root — a startup that fails
    /// later must never leave an unignored directory behind. Callers gate
    /// this on `root` being clice's own dedicated directory (the default
    /// `${workspace}/.clice`) — an explicitly configured directory may be
    /// shared with other content, which the markers would hide from git
    /// and skip in backups. Best effort; an existing marker (possibly
    /// user-customized) is never touched.
    static void write_ignore_markers(llvm::StringRef root);

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

    /// Return the key's aux blob path when the entry exists and its aux
    /// blob was committed.  Also refreshes last-accessed.  A miss with a
    /// present primary means the pair is incomplete (crash between the two
    /// commits, failed aux commit) — callers treat it as a cache miss and
    /// rebuild the pair.
    std::optional<std::string> lookup_aux(llvm::StringRef ns, llvm::StringRef key);

    /// Begin a two-phase write: returns a unique tmp path the blob must be
    /// written to (safe to hand to a worker process).
    PendingEntry begin_store(llvm::StringRef ns, llvm::StringRef key);

    /// Begin a two-phase write of the key's aux blob.  Commit the primary
    /// first: committing an aux blob for a key with no live entry fails.
    PendingEntry begin_store_aux(llvm::StringRef ns, llvm::StringRef key);

    /// Finish a two-phase write: fsync the tmp file and atomically rename
    /// it to its final path.  Triggers LRU eviction when the namespace
    /// exceeds its budget.  Returns the final blob path.
    ///
    /// On a rename collision (Windows, destination open) the existing blob
    /// is kept only when verified byte-identical to the new one; otherwise
    /// the stale destination is removed and the rename retried, and if the
    /// new blob still cannot be published an error is returned.
    ///
    /// `binding`, when set, receives the committed blob's identity (see
    /// BlobBinding) — read and stat'ed from the tmp file before the
    /// rename, or from the byte-identical survivor of a benign collision.
    /// Durable commits also drop this writer's dirty marker first (see
    /// mark_writer_dirty).
    std::expected<std::string, std::error_code> commit(PendingEntry pending,
                                                       BlobBinding* binding = nullptr);

    /// Persist a durable "this writer has published blobs whose metadata
    /// may not have reached the database yet" marker in this instance's
    /// tmp directory, before the publication it covers. Cleared by
    /// clear_writer_dirty once the metadata barrier completes; a crash in
    /// between leaves the marker for the next open() to find. Returns the
    /// mark count (see clear_writer_dirty).
    std::uint64_t mark_writer_dirty();

    /// Drop the dirty marker — but only when no publication happened since
    /// the caller observed mark count `upto`: a blob committed while the
    /// metadata barrier was in flight is not covered by it.
    void clear_writer_dirty(std::uint64_t upto);

    /// Marks issued so far (monotonic); pair with clear_writer_dirty.
    std::uint64_t writer_mark_count() const;

    /// Whether open() found the dirty marker of a dead writer: that
    /// session crashed between publishing blobs and persisting their
    /// metadata, so records adopted from the database must verify their
    /// blob's content hash on first use.
    bool dead_writer_dirty() const;

    /// Remove a blob.  Primarily for Persistent namespaces, whose cleanup
    /// is the caller's mark-and-sweep; LRU namespaces rarely need it.
    void invalidate(llvm::StringRef ns, llvm::StringRef key);

    /// Enumerate all keys in a namespace (for caller-side mark-and-sweep).
    /// Iterates over a snapshot, so fn may call back into the store.
    void for_each_key(llvm::StringRef ns, llvm::function_ref<void(llvm::StringRef)> fn);

    /// Number of in-flight tmp blobs of this instance (files under
    /// `tmp/{pid}`). A settled server has zero: every PendingEntry either
    /// committed (renamed away) or cleaned itself up. Memory/leak gauge
    /// for the stats endpoint; scans the directory, so not free.
    std::size_t pending_tmp_files() const;

    /// A blob the LRU budget evicted, reported so owners of derived
    /// in-memory state (e.g. the PCH cache's entry metadata) can drop it:
    /// eviction happens inside commit — possibly on a worker thread — so
    /// the store records instead of calling back, and the owner drains on
    /// its own loop.
    struct EvictedBlob {
        std::string ns;
        std::string key;
    };

    /// Drain the evictions recorded since the last call.
    std::vector<EvictedBlob> take_evictions();

    /// The versioned root directory, e.g. `{root}/cache/v1`.  Callers may
    /// place their own metadata files directly under it (the store only
    /// manages namespace subdirectories); they die with the version.
    llvm::StringRef base_dir() const;

    /// Whether the store was opened in read-only inspection mode.
    bool read_only() const;

    /// First namespace directory scan that failed since open (default
    /// error_code when none did).  A failed scan makes the namespace look
    /// empty while its blobs exist, so a reader that would report "no
    /// data" must check this first.  A read-only open of a namespace that
    /// was never created scans empty legitimately and is not a failure.
    std::error_code scan_error() const;

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
