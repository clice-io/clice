#include "index/database.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <type_traits>

#ifdef __linux__
#include <sys/vfs.h>
#endif

#include "lmdb.h"
#include "support/cache_store.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

namespace clice::index {

namespace {

constexpr llvm::StringLiteral index_lock_name = "index.lock";

/// Cross-process writer lock for the index lineage, shared by both
/// backends and always taken before the backend touches any of its files.
/// Atomic per-blob replacement (or a database file) cannot serialize the
/// mutable global/manifest lineage: two writers (an LSP server plus a
/// batch `clice index`) derive the same next generation from the same
/// loaded state, so a manifest written by one passes the other's
/// generation pin with FileVersion ids allocated against a different
/// table, loading rows under the wrong files. An OS advisory lock dies
/// with its process, so a crash leaves nothing stale behind.
std::optional<int> acquire_writer_lock(CacheStore& store) {
    auto lock_path = path::join(store.base_dir(), index_lock_name);
    int lock_fd = -1;
    if(auto ec = llvm::sys::fs::openFileForReadWrite(lock_path,
                                                     lock_fd,
                                                     llvm::sys::fs::CD_OpenAlways,
                                                     llvm::sys::fs::OF_None)) {
        LOG_WARN("Failed to open the index writer lock {}: {}", lock_path, ec.message());
        return std::nullopt;
    }
    if(llvm::sys::fs::tryLockFile(lock_fd)) {
        LOG_WARN(
            "Another clice process is writing the index cache at {}; "
            "index persistence is disabled for this process",
            store.base_dir());
        llvm::sys::Process::SafelyCloseFileDescriptor(lock_fd);
        return std::nullopt;
    }
    return lock_fd;
}

void release_writer_lock(int lock_fd) {
    if(lock_fd != -1) {
        llvm::sys::fs::unlockFile(lock_fd);
        llvm::sys::Process::SafelyCloseFileDescriptor(lock_fd);
    }
}

// ── Filesystem backend ──────────────────────────────────────────────

llvm::StringRef namespace_of(IndexBlobKind kind) {
    switch(kind) {
        case IndexBlobKind::Shard: return "index";
        case IndexBlobKind::Manifest: return "index-manifest";
        case IndexBlobKind::Global: return "index-global";
        case IndexBlobKind::CDB: return "index-cdb";
    }
    std::unreachable();
}

class FsDatabase final : public BlobDatabase {
public:
    FsDatabase(CacheStore& store, int lock_fd) : store(store), lock_fd(lock_fd) {
        for(auto kind: {IndexBlobKind::Shard,
                        IndexBlobKind::Manifest,
                        IndexBlobKind::Global,
                        IndexBlobKind::CDB}) {
            store.register_namespace({
                .name = std::string(namespace_of(kind)),
                .extension = ".idx",
                .policy = CachePolicy::Persistent,
            });
        }
    }

    ~FsDatabase() override {
        release_writer_lock(lock_fd);
    }

    ReadBlob read(IndexBlobKind kind, llvm::StringRef key) override {
        auto path = store.lookup(namespace_of(kind), key);
        if(!path) {
            return {};
        }
        auto buffer = llvm::MemoryBuffer::getFile(*path);
        if(!buffer) {
            return {};
        }
        return {.buffer = std::move(*buffer)};
    }

    bool contains(IndexBlobKind kind, llvm::StringRef key) override {
        return store.lookup(namespace_of(kind), key).has_value();
    }

    llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                         llvm::ArrayRef<BlobKey> removes) override {
        // A failed batch keeps its removals too, mirroring the LMDB
        // backend's all-or-nothing commit: a removal landing without the
        // puts it was batched behind can delete a blob the surviving
        // on-disk state still references. load() re-sweeps whatever the
        // skip leaves behind.
        auto failed = write_puts(puts);
        if(!failed.empty()) {
            return failed;
        }
        for(auto& [kind, key]: removes) {
            store.invalidate(namespace_of(kind), key);
        }
        return {};
    }

    void for_each_key(IndexBlobKind kind, llvm::function_ref<void(llvm::StringRef)> fn) override {
        store.for_each_key(namespace_of(kind), fn);
    }

    std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
        return 0;
    }

    void retire_old_snapshot() override {}

    std::expected<bool, std::string> grow() override {
        return false;
    }

private:
    llvm::SmallVector<std::size_t> write_puts(llvm::ArrayRef<Blob> puts) {
        // Batch order encodes dependency (shards → manifests → global →
        // CDB snapshot), so the first failure fails the rest of the batch:
        // continuing would publish an entry whose prerequisites never
        // landed — e.g. a CDB snapshot vouching for a global that failed —
        // and load paths only tolerate a committed prefix, the crash shape.
        auto fail_from = [&](std::size_t i) {
            llvm::SmallVector<std::size_t> failed;
            for(; i < puts.size(); i += 1) {
                failed.push_back(i);
            }
            return failed;
        };
        for(std::size_t i = 0; i < puts.size(); i += 1) {
            auto& blob = puts[i];
            auto ns = namespace_of(blob.kind);
            auto pending = store.begin_store(ns, blob.key);
            std::error_code ec;
            llvm::raw_fd_ostream os(pending.tmp_path, ec);
            if(ec) {
                LOG_WARN("Failed to write index blob {}/{}: {}", ns, blob.key, ec.message());
                return fail_from(i);
            }
            os.write(blob.bytes.data(), blob.bytes.size());
            os.close();
            // A truncated blob (disk full) must never be committed: the
            // namespaces are Persistent, so it would be served forever.
            if(os.has_error()) {
                LOG_WARN("Failed to write index blob {}/{}: {}",
                         ns,
                         blob.key,
                         os.error().message());
                os.clear_error();
                return fail_from(i);
            }
            if(auto committed = store.commit(std::move(pending)); !committed) {
                LOG_WARN("Failed to commit index blob {}/{}: {}",
                         ns,
                         blob.key,
                         committed.error().message());
                return fail_from(i);
            }
        }
        return {};
    }

    CacheStore& store;
    int lock_fd;
};

// ── LMDB backend ────────────────────────────────────────────────────

constexpr llvm::StringLiteral lmdb_file_name = "index.mdb";

constexpr std::size_t lmdb_small_mapsize = 256ull << 20;

/// Virtual reservation; pages materialize on use. On POSIX the file's
/// size tracks the data high-water mark, so the reservation is generous.
/// On Windows the mapping extends the file to the whole mapsize — a
/// 64 GiB file (sparse or not) alarms users and feeds backup and sync
/// tools at its logical size — so the map starts small and grows on
/// demand instead.
#ifdef _WIN32
constexpr std::size_t lmdb_default_mapsize = lmdb_small_mapsize;
#else
constexpr std::size_t lmdb_default_mapsize = 64ull << 30;
#endif

char kind_prefix(IndexBlobKind kind) {
    switch(kind) {
        case IndexBlobKind::Shard: return 'S';
        case IndexBlobKind::Manifest: return 'M';
        case IndexBlobKind::Global: return 'G';
        case IndexBlobKind::CDB: return 'C';
    }
    std::unreachable();
}

llvm::SmallString<64> encode_key(IndexBlobKind kind, llvm::StringRef key) {
    llvm::SmallString<64> encoded;
    encoded.push_back(kind_prefix(kind));
    encoded.append(key);
    return encoded;
}

/// The environment's self-description, stored under a key no kind prefix
/// can produce. LMDB files are not portable across word sizes or byte
/// orders and carry no schema of ours — any mismatch discards the whole
/// database (it is a rebuildable cache).
constexpr llvm::StringLiteral meta_key = "\xffmeta";
constexpr std::uint32_t lmdb_schema_version = 1;

struct MetaRecord {
    std::uint32_t schema = lmdb_schema_version;
    std::uint32_t word_bits = sizeof(std::size_t) * 8;
    std::uint32_t byte_order = 0x01020304;
};

// The record is compared with memcmp; padding or a surprising layout
// would poison every comparison.
static_assert(sizeof(MetaRecord) == 12 && std::has_unique_object_representations_v<MetaRecord>);

MDB_val to_val(llvm::StringRef bytes) {
    return {bytes.size(), const_cast<char*>(bytes.data())};
}

// MDB_PAGE_NOTFOUND is not a key miss: a page the B-tree references is
// absent from the file, which LMDB documents as corruption.
bool is_corruption(int rc) {
    return rc == MDB_CORRUPTED || rc == MDB_INVALID || rc == MDB_VERSION_MISMATCH ||
           rc == MDB_PAGE_NOTFOUND;
}

void remove_database_files(llvm::StringRef path) {
    llvm::sys::fs::remove(path);
    llvm::sys::fs::remove(path + "-lock");
}

class LmdbDatabase final : public BlobDatabase {
public:
    LmdbDatabase(MDB_env* env, MDB_dbi dbi, MDB_txn* txn, std::string path, int lock_fd) :
        env(env), dbi(dbi), txn(txn), path(std::move(path)), lock_fd(lock_fd) {}

    ~LmdbDatabase() override {
        retire_old_snapshot();
        if(txn) {
            mdb_txn_abort(txn);
        }
        mdb_env_close(env);
        // Condemned = corruption observed at read time; deleting under the
        // writer lock lets the next open start from an empty database.
        if(condemned) {
            remove_database_files(path);
        }
        release_writer_lock(lock_fd);
    }

    ReadBlob read(IndexBlobKind kind, llvm::StringRef key) override {
        if(!txn) {
            return {};
        }
        auto encoded = encode_key(kind, key);
        auto mk = to_val(encoded);
        MDB_val value;
        if(int rc = mdb_get(txn, dbi, &mk, &value)) {
            note_error(rc);
            return {};
        }
        llvm::StringRef bytes(static_cast<const char*>(value.mv_data), value.mv_size);
        // Index blobs need an 8-aligned base (their largest scalar is
        // u64 and the flatbuffers verifier checks offsets relative to
        // it). LMDB only guarantees that for overflow-page values; small
        // inline values are copied into an owned, allocator-aligned
        // buffer and become immortal (generation 0).
        if(reinterpret_cast<std::uintptr_t>(value.mv_data) % 8 != 0) {
            return {.buffer = llvm::MemoryBuffer::getMemBufferCopy(bytes)};
        }
        return {.buffer = llvm::MemoryBuffer::getMemBuffer(bytes,
                                                           "",
                                                           /*RequiresNullTerminator=*/false),
                .generation = generation};
    }

    bool contains(IndexBlobKind kind, llvm::StringRef key) override {
        if(!txn) {
            return false;
        }
        auto encoded = encode_key(kind, key);
        auto mk = to_val(encoded);
        MDB_val value;
        // Any error other than "not found" counts as present-but-unreadable;
        // the loader reads corrupted() to decide rebuild vs touch-nothing.
        int rc = mdb_get(txn, dbi, &mk, &value);
        note_error(rc);
        return rc != MDB_NOTFOUND;
    }

    llvm::SmallVector<std::size_t> write(llvm::ArrayRef<Blob> puts,
                                         llvm::ArrayRef<BlobKey> removes) override {
        auto fail_all = [&](int rc, llvm::StringRef stage) {
            if(rc == MDB_MAP_FULL) {
                map_full.store(true, std::memory_order_relaxed);
            }
            note_error(rc);
            LOG_WARN("Index database write failed ({}): {}", stage, mdb_strerror(rc));
            llvm::SmallVector<std::size_t> failed;
            for(std::size_t i = 0; i < puts.size(); i += 1) {
                failed.push_back(i);
            }
            return failed;
        };
        MDB_txn* wtxn = nullptr;
        if(int rc = mdb_txn_begin(env, nullptr, 0, &wtxn)) {
            return fail_all(rc, "begin");
        }
        for(auto& blob: puts) {
            auto encoded = encode_key(blob.kind, blob.key);
            auto mk = to_val(encoded);
            auto mv = to_val(blob.bytes);
            if(int rc = mdb_put(wtxn, dbi, &mk, &mv, 0)) {
                mdb_txn_abort(wtxn);
                return fail_all(rc, "put");
            }
        }
        for(auto& [kind, key]: removes) {
            auto encoded = encode_key(kind, key);
            auto mk = to_val(encoded);
            if(int rc = mdb_del(wtxn, dbi, &mk, nullptr); rc != 0 && rc != MDB_NOTFOUND) {
                mdb_txn_abort(wtxn);
                return fail_all(rc, "del");
            }
        }
        if(int rc = mdb_txn_commit(wtxn)) {
            return fail_all(rc, "commit");
        }
        return {};
    }

    void for_each_key(IndexBlobKind kind, llvm::function_ref<void(llvm::StringRef)> fn) override {
        if(!txn) {
            return;
        }
        MDB_cursor* cursor = nullptr;
        if(int rc = mdb_cursor_open(txn, dbi, &cursor)) {
            note_error(rc);
            return;
        }
        char prefix = kind_prefix(kind);
        MDB_val key{1, &prefix};
        MDB_val value;
        int rc = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
        while(rc == 0) {
            llvm::StringRef bytes(static_cast<const char*>(key.mv_data), key.mv_size);
            if(bytes.empty() || bytes[0] != prefix) {
                break;
            }
            fn(bytes.drop_front());
            rc = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
        }
        if(rc != 0 && rc != MDB_NOTFOUND) {
            note_error(rc);
        }
        mdb_cursor_close(cursor);
    }

    std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
        MDB_txn* fresh = nullptr;
        if(int rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &fresh)) {
            note_error(rc);
            return std::unexpected(std::string(mdb_strerror(rc)));
        }
        // No predecessor to retire when a grow() resized the map but
        // failed to reopen a snapshot.
        if(txn) {
            outstanding.push_back(txn);
        }
        txn = fresh;
        generation += 1;
        return generation;
    }

    void retire_old_snapshot() override {
        for(auto* old: outstanding) {
            mdb_txn_abort(old);
        }
        outstanding.clear();
    }

    std::expected<bool, std::string> grow() override {
        if(!map_full.load(std::memory_order_relaxed)) {
            return false;
        }
        // set_mapsize demands no live transaction in this process, so
        // every snapshot dies here; the caller owns rebinding all
        // borrowers before the next suspension point.
        retire_old_snapshot();
        if(txn) {
            mdb_txn_abort(txn);
            txn = nullptr;
        }
        MDB_envinfo info;
        mdb_env_info(env, &info);
        auto grown = std::max<std::size_t>(info.me_mapsize * 2, lmdb_small_mapsize);
        std::string error;
        if(int rc = mdb_env_set_mapsize(env, grown)) {
            error = mdb_strerror(rc);
        } else {
            map_full.store(false, std::memory_order_relaxed);
        }
        MDB_txn* fresh = nullptr;
        if(int rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &fresh)) {
            note_error(rc);
            // No snapshot at all: reads degrade to "missing" until a
            // later grow() succeeds. Persistence stays attached — closing
            // the environment would invalidate every borrowed buffer.
            return std::unexpected(error.empty() ? std::string(mdb_strerror(rc)) : error);
        }
        txn = fresh;
        generation += 1;
        if(!error.empty()) {
            return std::unexpected(error);
        }
        return true;
    }

    bool corrupted() const override {
        return poisoned.load(std::memory_order_relaxed);
    }

    void condemn() override {
        condemned = true;
    }

private:
    /// Latch corruption-family read errors for the loader's rebuild
    /// decision; everything else stays "missing or transiently unreadable".
    void note_error(int rc) {
        if(is_corruption(rc)) {
            poisoned.store(true, std::memory_order_relaxed);
        }
    }

    MDB_env* env;
    MDB_dbi dbi;
    /// Current read snapshot; owned by the opening (event-loop) thread.
    MDB_txn* txn;
    /// Pre-advance snapshots kept alive while their borrowers migrate; a
    /// migration cancelled mid-way leaves entries for the next one.
    llvm::SmallVector<MDB_txn*, 2> outstanding;
    std::uint64_t generation = 1;
    std::string path;
    /// Set by write() on the pool thread, consumed by grow() on the loop.
    std::atomic<bool> map_full = false;
    /// See note_error()/corrupted(); written on both loop and pool threads.
    std::atomic<bool> poisoned = false;
    bool condemned = false;
    int lock_fd;
};

#ifdef _WIN32
/// Without the sparse attribute Windows backs the whole mapsize with real
/// disk (CreateFileMapping allocates eagerly), so the file is marked
/// sparse before LMDB first maps it. Best effort: on a volume without
/// sparse support each mapsize is simply committed up front, which the
/// small default keeps tolerable.
void make_sparse(llvm::StringRef path) {
    int fd = -1;
    if(llvm::sys::fs::openFileForReadWrite(path,
                                           fd,
                                           llvm::sys::fs::CD_OpenAlways,
                                           llvm::sys::fs::OF_None)) {
        return;
    }
    auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    DWORD returned = 0;
    DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
    llvm::sys::Process::SafelyCloseFileDescriptor(fd);
}
#endif

enum class MetaCheck : std::uint8_t {
    Ok,
    /// Foreign word size/endianness/schema, or data without any meta —
    /// the corruption-recovery shape (delete and rebuild when writable).
    Mismatch,
    /// Could not validate right now; touch nothing.
    Transient,
};

/// Validates the meta record under the resident transaction; initializes
/// it first on a fresh (provably empty) writable database.
MetaCheck check_meta(MDB_env* env, MDB_dbi dbi, MDB_txn* txn, bool read_only) {
    auto mk = to_val(meta_key);
    MDB_val value;
    int rc = mdb_get(txn, dbi, &mk, &value);
    MetaRecord expected;
    if(rc == 0) {
        bool ok = value.mv_size == sizeof(MetaRecord) &&
                  std::memcmp(value.mv_data, &expected, sizeof(MetaRecord)) == 0;
        return ok ? MetaCheck::Ok : MetaCheck::Mismatch;
    }
    if(rc != MDB_NOTFOUND) {
        return is_corruption(rc) ? MetaCheck::Mismatch : MetaCheck::Transient;
    }
    // Meta missing: only a provably EMPTY database may be initialized in
    // place — populated bytes without our meta are a foreign layout, and
    // stamping them would bypass the schema gate.
    MDB_stat stat;
    if(mdb_stat(txn, dbi, &stat) != 0) {
        return MetaCheck::Transient;
    }
    if(stat.ms_entries != 0) {
        return MetaCheck::Mismatch;
    }
    if(read_only) {
        return MetaCheck::Ok;
    }
    MDB_txn* wtxn = nullptr;
    if(mdb_txn_begin(env, nullptr, 0, &wtxn) != 0) {
        return MetaCheck::Transient;
    }
    MDB_val mv{sizeof(MetaRecord), &expected};
    if(mdb_put(wtxn, dbi, &mk, &mv, 0) != 0) {
        mdb_txn_abort(wtxn);
        return MetaCheck::Transient;
    }
    return mdb_txn_commit(wtxn) == 0 ? MetaCheck::Ok : MetaCheck::Transient;
}

std::unique_ptr<LmdbDatabase> open_lmdb_env(CacheStore& store,
                                            int lock_fd,
                                            std::size_t initial_mapsize) {
    auto path = path::join(store.base_dir(), lmdb_file_name);
    bool read_only = store.read_only();

    auto mapsize = initial_mapsize != 0 ? initial_mapsize : lmdb_default_mapsize;

    // One recovery retry: confirmed corruption (or a meta mismatch) is
    // repaired by deleting the database — it is a rebuildable cache, and
    // the writer lock guarantees no other live process is using it.
    // Transient errors (permissions, fd/memory pressure) must NOT delete
    // anything: persistence is disabled for this session instead, the
    // same discipline the loader applies to an unreadable global blob.
    for(int attempt = 0; attempt < 2; attempt += 1) {
        // Giving up must not leave behind a file this attempt created
        // (make_sparse and mdb_env_open both create on demand): read-only
        // opens select the LMDB backend on bare existence, so an abandoned
        // uninitialized placeholder would shadow the per-file blobs.
        bool created = !read_only && !llvm::sys::fs::exists(path);
        auto discard_created = [&] {
            if(created) {
                remove_database_files(path);
            }
        };
#ifdef _WIN32
        if(!read_only) {
            make_sparse(path);
        }
#endif
        MDB_env* env = nullptr;
        if(int rc = mdb_env_create(&env)) {
            LOG_WARN("Failed to create the index database environment: {}", mdb_strerror(rc));
            discard_created();
            return nullptr;
        }
        mdb_env_set_mapsize(env, mapsize);
        unsigned flags = MDB_NOSUBDIR | MDB_NOTLS | (read_only ? MDB_RDONLY : 0);
        // True = the failure was repaired by deleting the database and the
        // loop should retry; false = give up with persistence disabled.
        auto fail = [&](int rc, llvm::StringRef stage) {
            mdb_env_close(env);
            if(!read_only && is_corruption(rc) && attempt == 0) {
                LOG_WARN("Index database at {} is corrupt ({} failed: {}); rebuilding",
                         path,
                         stage,
                         mdb_strerror(rc));
                remove_database_files(path);
                return true;
            }
            LOG_WARN(
                "Cannot open the index database at {} ({} failed: {}); "
                "index persistence is disabled for this session",
                path,
                stage,
                mdb_strerror(rc));
            discard_created();
            return false;
        };

        if(int rc = mdb_env_open(env, path.c_str(), flags, 0644)) {
            if(fail(rc, "open")) {
                continue;
            }
            return nullptr;
        }
        if(!read_only) {
            int dead = 0;
            mdb_reader_check(env, &dead);
            if(dead != 0) {
                LOG_INFO("Cleared {} stale index database readers", dead);
            }
        }
        MDB_txn* txn = nullptr;
        int rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn);
        if(rc == MDB_MAP_RESIZED) {
            // Another process grew the map between open and here; adopt
            // its size (no transaction is active yet) and retry once.
            mdb_env_set_mapsize(env, 0);
            rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn);
        }
        if(rc != 0) {
            if(fail(rc, "snapshot")) {
                continue;
            }
            return nullptr;
        }
        MDB_dbi dbi;
        if(int dbi_rc = mdb_dbi_open(txn, nullptr, 0, &dbi)) {
            mdb_txn_abort(txn);
            if(fail(dbi_rc, "dbi")) {
                continue;
            }
            return nullptr;
        }
        switch(check_meta(env, dbi, txn, read_only)) {
            case MetaCheck::Ok: break;
            case MetaCheck::Mismatch: {
                mdb_txn_abort(txn);
                if(fail(MDB_INVALID, "meta")) {
                    continue;
                }
                return nullptr;
            }
            case MetaCheck::Transient: {
                mdb_txn_abort(txn);
                mdb_env_close(env);
                LOG_WARN(
                    "Cannot validate the index database at {}; "
                    "index persistence is disabled for this session",
                    path);
                discard_created();
                return nullptr;
            }
        }
        return std::make_unique<LmdbDatabase>(env, dbi, txn, std::move(path), lock_fd);
    }
    return nullptr;
}

enum class FsLocality : std::uint8_t {
    Local,
    Remote,
    /// FUSE fronts anything from a local overlay to sshfs; LMDB may or
    /// may not survive there — warn and respect the configuration.
    Unknown,
};

FsLocality filesystem_locality(llvm::StringRef dir) {
#ifdef __linux__
    // llvm's is_local only knows NFS/SMB/CIFS on Linux; 9p (WSL drvfs
    // mounts) and FUSE pass as local, and those are exactly the mounts
    // LMDB is documented to break on.
    struct statfs sfs;
    if(statfs(std::string(dir).c_str(), &sfs) == 0) {
        // Through uint32 first: f_type is a signed word, and the SMB2/CIFS
        // magics have the top bit set.
        switch(static_cast<std::uint32_t>(sfs.f_type)) {
            case 0x6969:      // NFS
            case 0x517B:      // SMB
            case 0xFE534D42:  // SMB2
            case 0xFF534D42:  // CIFS
            case 0x01021997:  // 9p
                return FsLocality::Remote;
            case 0x65735546:  // FUSE
                return FsLocality::Unknown;
            default: break;
        }
    }
#endif
    bool local = true;
    if(auto ec = llvm::sys::fs::is_local(dir, local)) {
        // Undetermined counts as local: an over-eager fallback would
        // silently fork the index lineage.
        LOG_WARN("Cannot determine whether {} is a local filesystem ({}); assuming local",
                 dir,
                 ec.message());
        return FsLocality::Local;
    }
    return local ? FsLocality::Local : FsLocality::Remote;
}

}  // namespace

std::unique_ptr<BlobDatabase> open_fs_database(CacheStore& store) {
    int lock_fd = -1;
    if(!store.read_only()) {
        auto locked = acquire_writer_lock(store);
        if(!locked) {
            return nullptr;
        }
        lock_fd = *locked;
    }
    return std::make_unique<FsDatabase>(store, lock_fd);
}

std::unique_ptr<BlobDatabase> open_lmdb_database(CacheStore& store, std::size_t initial_mapsize) {
    int lock_fd = -1;
    if(!store.read_only()) {
        auto locked = acquire_writer_lock(store);
        if(!locked) {
            return nullptr;
        }
        lock_fd = *locked;
    }
    auto db = open_lmdb_env(store, lock_fd, initial_mapsize);
    if(!db) {
        release_writer_lock(lock_fd);
    }
    return db;
}

std::unique_ptr<BlobDatabase> open_database(CacheStore& store, llvm::StringRef backend) {
    if(backend == "files") {
        return open_fs_database(store);
    }
    if(backend != "lmdb") {
        LOG_WARN("Unknown index_db backend '{}'; using lmdb", backend);
    }
    switch(filesystem_locality(store.base_dir())) {
        case FsLocality::Local: break;
        case FsLocality::Remote: {
            LOG_WARN(
                "{} is on a remote filesystem, which LMDB does not support; "
                "using per-file index storage",
                store.base_dir());
            return open_fs_database(store);
        }
        case FsLocality::Unknown: {
            LOG_WARN(
                "{} is on a FUSE filesystem; LMDB needs local-filesystem semantics — "
                "set index_db = \"files\" if the index database misbehaves",
                store.base_dir());
            break;
        }
    }
    // A reader before any LMDB writer ever ran (or after "files" runs)
    // reads whatever the per-file backend left behind — including nothing.
    if(store.read_only() && !llvm::sys::fs::exists(path::join(store.base_dir(), lmdb_file_name))) {
        return open_fs_database(store);
    }
    return open_lmdb_database(store);
}

}  // namespace clice::index
