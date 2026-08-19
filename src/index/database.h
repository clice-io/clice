#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"

namespace clice {

class CacheStore;

}

namespace clice::index {

/// The blob families the index persists.
enum class IndexBlobKind : std::uint8_t {
    /// Per-file row blobs (ShardBlob), keyed by a path hash.
    Shard,
    /// Per-TU manifests, keyed by a path hash.
    Manifest,
    /// The single global blob (FileVersion table + symbols), key "global".
    Global,
    /// The single CDB command-hash snapshot the persisted index was built
    /// against, key "cdb" — how a cold start detects compile-command
    /// changes that happened while no server was running.
    CDB,
};

/// One blob read out of the database. The bytes' lifetime is
/// backend-defined, and `generation` says which contract applies:
///
/// - 0: the buffer owns its bytes (filesystem backend, and small LMDB
///   values copied out for alignment) — valid for the buffer's lifetime.
/// - nonzero: the bytes are borrowed from the backend's read snapshot of
///   that generation and die when it is retired (advance_read_snapshot()
///   followed by retire_old_snapshot(), or grow()). A caller keeping such
///   bytes across a save must re-read them after every snapshot advance.
struct ReadBlob {
    std::unique_ptr<llvm::MemoryBuffer> buffer;
    std::uint64_t generation = 0;

    explicit operator bool() const {
        return buffer != nullptr;
    }
};

/// Key of one persisted blob, for batch removals.
struct BlobKey {
    IndexBlobKind kind;
    std::string key;
};

/// Keyed blob database of the persisted index.
///
/// Boundary with CacheStore: CacheStore manages the versioned cache
/// directory and file-shaped artifacts (PCH/PCM/header contexts — clang
/// consumes those by path, so they must be real files); BlobDatabase is
/// the index's keyed blob store living inside that directory. The
/// filesystem backend materializes each blob as one CacheStore-namespace
/// file; the LMDB backend keeps them all in a single `index.mdb`.
///
/// `write` does the heavy IO and belongs off the event loop; every other
/// method is cheap and event-loop-only (read snapshots are owned by the
/// opening thread).
class BlobDatabase {
public:
    virtual ~BlobDatabase() = default;

    /// The blob's bytes, or a null ReadBlob when missing or unreadable.
    /// See ReadBlob for the lifetime contract.
    virtual ReadBlob read(IndexBlobKind kind, llvm::StringRef key) = 0;

    /// Whether a blob exists under the key, even when unreadable — how the
    /// loader tells a missing global blob (sweep everything) from a
    /// transient read failure (touch nothing).
    virtual bool contains(IndexBlobKind kind, llvm::StringRef key) = 0;

    struct Blob {
        IndexBlobKind kind;
        std::string key;
        std::string bytes;
    };

    /// Persist `puts` in order, then delete `removes`. Atomicity is
    /// backend-defined but never weaker than a committed prefix of `puts`:
    /// batch order encodes dependency (shards → manifests → global → CDB
    /// snapshot), load paths tolerate exactly a prefix, and the LMDB
    /// backend commits everything or nothing in one transaction, removes
    /// included. The failed put indices are returned so the caller can
    /// re-dirty them; removes are best-effort (load sweeps stale blobs by
    /// their generation pins). A full LMDB map fails the whole batch and
    /// latches the grow request `grow()` serves.
    virtual llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                                 llvm::ArrayRef<BlobKey> removes) = 0;

    virtual void for_each_key(IndexBlobKind kind, llvm::function_ref<void(llvm::StringRef)> fn) = 0;

    /// Open a new read snapshot and return its generation; reads from now
    /// on come from it. The previous snapshot — and every borrowed buffer
    /// read out of it — stays valid until retire_old_snapshot(), so the
    /// caller can migrate long-lived borrowers incrementally, yielding
    /// between batches. On failure the current snapshot keeps serving and
    /// nothing is invalidated. No-op on backends without snapshots
    /// (filesystem): returns the current generation, 0.
    virtual std::expected<std::uint64_t, std::string> advance_read_snapshot() = 0;

    /// Retire every snapshot advance_read_snapshot() has replaced,
    /// invalidating the buffers still borrowed from them. A migration
    /// cancelled between advance and retire leaves snapshots outstanding;
    /// the next migration rebinds every borrower onto the newest snapshot
    /// and retires them all. No-op when nothing is pending.
    virtual void retire_old_snapshot() = 0;

    /// Resize the map after write() failed on a full map, retiring EVERY
    /// read snapshot first — all borrowed buffers die immediately — and
    /// opening a fresh one. Returns true when the map was resized: the
    /// caller must then rebind every borrower before yielding the event
    /// loop. False when no growth was pending (including the filesystem
    /// backend, where this is a no-op). An error return also means growth
    /// was attempted, so every snapshot has already been retired: borrowed
    /// buffers are dead and must be shed or re-read, and a replacement
    /// snapshot may not have opened — reads then miss until a later grow()
    /// succeeds.
    virtual std::expected<bool, std::string> grow() = 0;

    /// Whether the backend observed page-level corruption after open
    /// (reads failing with LMDB's corruption family). The loader uses it
    /// to tell "rebuild me" apart from a transient failure; backends
    /// without the concept never latch it.
    virtual bool corrupted() const {
        return false;
    }

    /// Mark the database for deletion when it closes — the recovery for
    /// corruption observed at read time. The files go away under the
    /// writer lock, so the next open starts from an empty database.
    virtual void condemn() {}
};

/// Filesystem backend over the cache store; registers the index
/// namespaces on construction. On a writable store this takes an exclusive
/// cross-process writer lock for the index (held until destruction) and
/// returns nullptr when another clice process already holds it — the
/// global/manifest blobs form one mutable lineage that tolerates no second
/// writer. Read-only stores skip the lock.
std::unique_ptr<BlobDatabase> open_fs_database(CacheStore& store);

/// LMDB backend: a single `index.mdb` (plus its `-lock` file) in the
/// store's version directory. Takes the same writer lock as the
/// filesystem backend before touching the environment. Returns nullptr
/// when the lock is held elsewhere or the environment cannot be opened
/// safely — only confirmed corruption (or a meta mismatch) is repaired by
/// deleting and rebuilding the database; transient errors disable index
/// persistence for the session and touch nothing. A read-only open uses
/// MDB_RDONLY, which still registers a reader slot in the `-lock` file —
/// the one deviation from the cache store's read-only-touches-nothing
/// contract. `initial_mapsize` overrides the default virtual map
/// reservation (tests exercise the growth path with a tiny map); 0 keeps
/// the default.
std::unique_ptr<BlobDatabase> open_lmdb_database(CacheStore& store,
                                                 std::size_t initial_mapsize = 0);

/// Backend selection: `backend` is the `project.index_db` config value
/// ("lmdb" or "files"). Remote filesystems (which LMDB cannot run on) fall
/// back to the filesystem backend with a warning; an LMDB environment that
/// cannot be opened safely disables persistence (nullptr) rather than
/// falling back — a second lineage of filesystem blobs next to a live
/// index.mdb would split the index's history.
std::unique_ptr<BlobDatabase> open_database(CacheStore& store, llvm::StringRef backend);

}  // namespace clice::index
