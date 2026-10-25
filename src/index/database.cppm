module;

#include "support/prelude.h"

module clice:index.database;

import :vfs.cache_store;

namespace clice::index {

/// A key holding `value` big-endian, so keys sort as the numbers do.
template <typename T>
std::string number_key(T value) {
    static_assert(std::is_unsigned_v<T>);
    std::string key(sizeof(T), '\0');
    for(std::uint32_t i = 0; i < sizeof(T); i += 1) {
        key[i] = static_cast<char>(value >> (8 * (sizeof(T) - 1 - i)));
    }
    return key;
}

/// The number a number_key holds; nullopt for a key of another width.
template <typename T>
std::optional<T> key_number(llvm::StringRef key) {
    if(key.size() != sizeof(T)) {
        return std::nullopt;
    }
    T value = 0;
    for(auto byte: key) {
        value = static_cast<T>((value << 8) | static_cast<std::uint8_t>(byte));
    }
    return value;
}

/// What names a file in the database: the hash of its name there
/// (ProjectIndex::portable), since runtime ids are per-session. Producers in
/// instantiation blobs name their unit by the same hash.
inline std::uint64_t path_hash(llvm::StringRef name) {
    return llvm::xxh3_64bits(name);
}

/// The record families of the index database. File-keyed ones take a
/// number_key of the file's path_hash; symbol-keyed ones of the symbol hash.
enum class BlobKind : std::uint8_t {
    /// Per-file shard blobs (ShardBlob).
    Shard,
    /// Per-file instantiation blobs (InstantiationShardBlob).
    InstantiationShard,
    /// Per-TU manifests (TUManifestBlob), keyed by the TU's file.
    TUManifest,
    /// The single header (HeaderBlob), key "header".
    Header,
    /// Path id (a number_key) -> the file's name, the raw bytes.
    Path,
    /// Version id (a number_key) -> the file version manifests name by it
    /// (PersistedVersion).
    Version,
    /// Per-file records (FileBlob): what the start-up scan reads.
    File,
    /// Per-file records (DependentsBlob): the units depending on the file.
    Dependents,
    /// Per-file records (InternalsBlob): the names of internal linkage the
    /// file declares, for the search index.
    Internals,
    /// Per-symbol records (encode_record) of the external symbols.
    Symbol,
    /// Per-symbol records (SharedLocalBlob) of the symbols below external
    /// scope several files hold.
    SharedLocal,
    /// The single CDB command-hash snapshot the persisted index was built
    /// against, key "cdb" — how a cold start detects compile-command
    /// changes that happened while no server was running.
    CommandSnapshot,
    /// The single artifact records blob (PCH/PCM records with their deps,
    /// header-mode results), key "artifacts".
    ArtifactRecords,
    /// The single context choices blob (saved context choices, synthesized
    /// artifact hosts), key "contexts" — sovereignty records, never
    /// garbage-collected by content.
    ContextChoices,
    /// The segments of the name search index (index/search_index.cppm)
    /// derived from the symbol records, keyed by a number_key of the
    /// segment id the header lists.
    SearchSegment,
};

/// One blob read out of the database. `generation` says which lifetime
/// contract applies:
///
/// - 0: the buffer owns its bytes (small LMDB values copied out for
///   alignment) — valid for the buffer's lifetime.
/// - nonzero: the bytes are borrowed from the backend's read snapshot of
///   that generation and die when it is retired (advance_read_snapshot()
///   followed by retire_old_snapshot(), a grow(), or a swap_compact_copy()
///   that reopened the database). A caller keeping such
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
    BlobKind kind;
    std::string key;
};

/// How the pages of a database file are used.
struct PageUsage {
    std::uint64_t page_size = 0;
    /// Pages up to the last one the file has used: the file's size, which
    /// writes never shrink.
    std::uint64_t pages = 0;
    /// B-tree pages: the keys, and the values small enough to sit beside them.
    std::uint64_t tree_pages = 0;
    /// The values too large for a tree page, each in a run of whole pages.
    std::uint64_t overflow_pages = 0;
    /// Pages earlier writes freed, kept for later writes to reuse.
    std::uint64_t free_pages = 0;
};

/// Keyed blob database of the persisted index.
///
/// Boundary with CacheStore: CacheStore manages the versioned cache
/// directory and file-shaped artifacts (PCH/PCM/header contexts — clang
/// consumes those by path, so they must be real files); BlobDatabase is
/// the index's keyed blob database inside that directory, a single
/// LMDB `index.mdb`. The interface stays virtual for test doubles (spies,
/// failure injection).
///
/// `write` does the heavy IO and belongs off the event loop; every other
/// method is cheap and event-loop-only (read snapshots are owned by the
/// opening thread).
class BlobDatabase {
public:
    virtual ~BlobDatabase() = default;

    /// The blob's bytes, or a null ReadBlob when missing or unreadable.
    /// See ReadBlob for the lifetime contract.
    virtual ReadBlob read(BlobKind kind, llvm::StringRef key) = 0;

    /// Whether a blob exists under the key, even when unreadable — how the
    /// loader tells a missing header (a first run) from a transient read
    /// failure (touch nothing).
    virtual bool contains(BlobKind kind, llvm::StringRef key) = 0;

    struct Blob {
        BlobKind kind;
        std::string key;
        std::vector<std::byte> bytes;
    };

    /// Persist `puts`, then delete `removes`, all or nothing: the records
    /// of the index reference each other, so a batch commits whole or not
    /// at all. False when nothing committed. A full LMDB map fails the
    /// batch and latches the grow request `grow()` serves.
    virtual bool write(llvm::ArrayRef<Blob> puts, llvm::ArrayRef<BlobKey> removes) = 0;

    virtual void for_each_key(BlobKind kind, llvm::function_ref<void(llvm::StringRef)> fn) = 0;

    /// Every record of `kind`, in key order; `fn` returns false to stop.
    /// The blobs follow ReadBlob's lifetime contract.
    virtual void for_each(BlobKind kind,
                          llvm::function_ref<bool(llvm::StringRef key, ReadBlob blob)> fn) = 0;

    /// How many records of `kind` the read snapshot holds.
    virtual std::uint32_t count(BlobKind kind) = 0;

    /// Open a new read snapshot and return its generation; reads from now
    /// on come from it. The previous snapshot — and every borrowed buffer
    /// read out of it — stays valid until retire_old_snapshot(), so the
    /// caller can migrate long-lived borrowers incrementally, yielding
    /// between batches. On failure the current snapshot keeps serving and
    /// nothing is invalidated.
    virtual std::expected<std::uint64_t, std::string> advance_read_snapshot() = 0;

    /// Retire every snapshot advance_read_snapshot() has replaced,
    /// invalidating the buffers still borrowed from them. A migration
    /// cancelled between advance and retire leaves snapshots outstanding;
    /// the next migration rebinds every borrower onto the newest snapshot
    /// and retires them all. No-op when nothing is pending.
    virtual void retire_old_snapshot() = 0;

    /// What grow() did.
    enum class Growth : std::uint8_t {
        /// No write had failed on a full map.
        None,
        /// The map was resized after retiring EVERY read snapshot — all
        /// borrowed buffers died — and a fresh one opened: the caller
        /// rebinds every borrower before yielding the event loop.
        Grown,
        /// The resize would fail — the address space cap or the free disk a
        /// mapping without sparse support allocates cannot take the larger
        /// map — so none was tried, since a failed resize leaves the
        /// environment unusable. Nothing changed; the batch stays unsaved.
        Refused,
    };

    /// Resize the map after write() failed on a full map. A resize that
    /// fails despite the headroom check ends the process with an error.
    virtual Growth grow() = 0;

    /// Write a copy of the read snapshot without the free pages beside the
    /// file, for swap_compact_copy(). Heavy IO, off the event loop; no write
    /// may commit until the swap. False when the copy failed (logged).
    virtual bool write_compact_copy() = 0;

    /// What swap_compact_copy() did.
    enum class Swap : std::uint8_t {
        /// Another process has the database open: the file stays, and the
        /// copy is gone.
        Busy,
        /// The handle was closed and opened again — on the copy, unless
        /// replacing the file failed (logged) — after retiring EVERY read
        /// snapshot: all borrowed buffers died, and the caller rebinds every
        /// borrower before yielding the event loop.
        Reopened,
        /// The handle could not be opened again and is unusable: the caller
        /// discards the database at once.
        Lost,
    };

    /// Replace the file by the copy write_compact_copy() wrote, when no other
    /// process has the database open. Swapping a file another process maps
    /// would mislead it: the copy restarts LMDB's transaction numbering,
    /// which the `-lock` file every opener shares records.
    virtual Swap swap_compact_copy() = 0;

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

    /// How the file's pages are used as of the read snapshot, for `clice
    /// index stats`; nullopt from backends without a file.
    virtual std::optional<PageUsage> usage() {
        return std::nullopt;
    }

    /// Whether this handle was opened read-only (a session that must not
    /// take the writer lock — batch lint, `clice index stats`). Writes
    /// are a caller bug then; persistence-side callers skip them.
    virtual bool read_only() const {
        return false;
    }
};

/// The directory of a build configuration's index database:
/// `index/<name>` under the store's version directory, every
/// configuration having a database of its own. The anonymous
/// configuration's is `default`; a tag's is its spelling lowercased and
/// reduced to `[a-z0-9_-]`, then `~` and a hash of the original. The hash
/// alone makes the name unique — filesystems fold case, Windows reserves
/// device names and strips trailing dots, and no tag may alias `default`
/// — while the prefix keeps it recognizable.
std::string database_directory(const CacheStore& store, llvm::StringRef configuration);

/// Whether the configuration's database holds its `index.mdb`, that is, an
/// index was ever built for it.
bool database_exists(const CacheStore& store, llvm::StringRef configuration);

/// The configuration's database: a single `index.mdb` (plus its `-lock`
/// file, and the `-open` file every reader holds locked while it has the
/// database open, which the writer locks exclusively to swap in a compact
/// copy) in its
/// database directory, created on demand. A writable open
/// presumes the caller holds the cache directory's writer lock
/// (index/writer_lock.cppm) — the records form one mutable lineage that
/// tolerates no second writer. Nullptr when the
/// environment cannot be opened safely — only confirmed corruption
/// (or a meta mismatch) is repaired by deleting and rebuilding the
/// database; transient errors disable index persistence for the session
/// and touch nothing. A read-only open of a database without an
/// `index.mdb` yet is nullptr too; an existing one opens with MDB_RDONLY, which
/// still registers a reader slot in the `-lock` file — the one deviation
/// from the cache store's read-only-touches-nothing contract.
/// `initial_mapsize` overrides the default virtual map reservation
/// (tests exercise the growth path with a tiny map); 0 keeps the default.
std::unique_ptr<BlobDatabase> open_lmdb_database(CacheStore& store,
                                                 llvm::StringRef configuration,
                                                 std::size_t initial_mapsize = 0,
                                                 bool read_only = false);

/// The production entry: open_lmdb_database gated on the store living on
/// a local filesystem. On a remote filesystem (NFS/SMB/9p — where LMDB is
/// documented to break) index persistence is disabled with a warning; a
/// FUSE mount gets the warning but proceeds, since FUSE fronts anything
/// from a local overlay to sshfs.
std::unique_ptr<BlobDatabase> open_database(CacheStore& store,
                                            llvm::StringRef configuration,
                                            bool read_only = false);

}  // namespace clice::index
