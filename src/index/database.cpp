module;

#include "modules/prelude.h"

#ifdef __linux__
#include <sys/vfs.h>
#endif

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif
#include "support/logging.macros.h"

module clice;

import :index.database;
import :index.format;
import :support.bytes;
import :support.logging;
import :support.text;
import :vfs.cache_store;
import :vfs.file_system;
import :vfs.path;

namespace clice::index {

namespace {

constexpr llvm::StringLiteral lmdb_file_name = "index.mdb";

constexpr std::size_t lmdb_small_mapsize = 256ull << 20;

/// Virtual reservation; pages materialize on use. On POSIX the file's
/// size tracks the data high-water mark, so the reservation is generous —
/// unless the address space is capped (`ulimit -v` on HPC and sandboxed
/// hosts), where it would not fit. On Windows the mapping extends the file
/// to the whole mapsize — a 64 GiB file (sparse or not) alarms users and
/// feeds backup and sync tools at its logical size. Either way the map
/// then starts small and grows on demand instead.
std::size_t default_mapsize() {
#ifdef _WIN32
    return lmdb_small_mapsize;
#else
    constexpr std::size_t generous = 64ull << 30;
    struct rlimit limit;
    if(getrlimit(RLIMIT_AS, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY &&
       limit.rlim_cur < 2 * generous) {
        return lmdb_small_mapsize;
    }
    return generous;
#endif
}

char kind_prefix(BlobKind kind) {
    switch(kind) {
        case BlobKind::Shard: return 'S';
        case BlobKind::InstantiationShard: return 'I';
        case BlobKind::TUManifest: return 'M';
        case BlobKind::Header: return 'H';
        case BlobKind::Path: return 'P';
        case BlobKind::File: return 'F';
        case BlobKind::Dependents: return 'D';
        case BlobKind::Internals: return 'T';
        case BlobKind::Symbol: return 'Y';
        case BlobKind::SharedLocal: return 'L';
        case BlobKind::CommandSnapshot: return 'C';
        case BlobKind::ArtifactRecords: return 'A';
        case BlobKind::ContextChoices: return 'X';
        case BlobKind::SearchIndex: return 'N';
    }
    std::unreachable();
}

llvm::SmallString<64> encode_key(BlobKind kind, llvm::StringRef key) {
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
constexpr std::uint32_t lmdb_schema_version = 2;

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

/// Readers are short-lived commands that may have been killed; a dead
/// one's slot pins its snapshot and the map only grows until it is cleared.
void clear_dead_readers(MDB_env* env) {
    int dead = 0;
    mdb_reader_check(env, &dead);
    if(dead != 0) {
        LOG_INFO("Cleared {} stale index database readers", dead);
    }
}

// MDB_PAGE_NOTFOUND is not a key miss: a page the B-tree references is
// absent from the file, which LMDB documents as corruption.
bool is_corruption(int rc) {
    return rc == MDB_CORRUPTED || rc == MDB_INVALID || rc == MDB_VERSION_MISMATCH ||
           rc == MDB_PAGE_NOTFOUND;
}

void remove_database_files(llvm::StringRef path) {
    vfs::remove(path);
    vfs::remove(path.str() + "-lock");
}

/// The file's length against the length its newest meta page guarantees:
/// every page up to the last one it declares.
struct Coverage {
    std::uint64_t length;
    std::uint64_t declared;
};

Coverage coverage(MDB_env* env) {
    MDB_envinfo info;
    mdb_env_info(env, &info);
    MDB_stat db_stat;
    mdb_env_stat(env, &db_stat);
    std::uint64_t declared = (static_cast<std::uint64_t>(info.me_last_pgno) + 1) * db_stat.ms_psize;
    mdb_filehandle_t fd;
    mdb_env_get_fd(env, &fd);
#ifdef _WIN32
    LARGE_INTEGER length{};
    ::GetFileSizeEx(fd, &length);
    return {static_cast<std::uint64_t>(length.QuadPart), declared};
#else
    struct stat file{};
    ::fstat(fd, &file);
    return {static_cast<std::uint64_t>(file.st_size), declared};
#endif
}

/// LMDB reads the file through a shared mapping, so a page referenced
/// past the end of a truncated file faults (SIGBUS) instead of failing
/// the read. A healthy file can be short too — a commit never writes the
/// tail pages it allocated and freed again, and nothing references them —
/// so the length alone cannot tell damage from health. Writers keep the
/// file covering every declared page, zero-filling the gap: a healthy
/// tree never reads those pages, and a damaged one reads zeros instead of
/// faulting — a tree page as MDB_CORRUPTED, which the repair path
/// handles, a blob as bytes its format check rejects. Read-only openers
/// rely on it and take a short file for damage; a file written before
/// this rule existed reads as damaged until a writer opens it. The write
/// transaction keeps every other writer from growing the file meanwhile.
int cover_declared_pages(MDB_env* env) {
#ifdef _WIN32
    // A writable mapping extends the file to the whole map size.
    return 0;
#else
    MDB_txn* txn = nullptr;
    if(int rc = mdb_txn_begin(env, nullptr, 0, &txn)) {
        return rc;
    }
    int rc = 0;
    if(auto [length, declared] = coverage(env); length < declared) {
        mdb_filehandle_t fd;
        mdb_env_get_fd(env, &fd);
        if(::ftruncate(fd, static_cast<off_t>(declared)) != 0) {
            rc = errno;
        }
    }
    mdb_txn_abort(txn);
    return rc;
#endif
}

/// Why mapping `extra` more bytes would fail, if it would: past the
/// address space cap (`ulimit -v`), or past the free disk a Windows file
/// without sparse support allocates the whole mapping from.
std::optional<std::string> growth_refused(MDB_env* env, std::uint64_t extra) {
#ifdef _WIN32
    mdb_filehandle_t fd;
    mdb_env_get_fd(env, &fd);
    FILE_BASIC_INFO basic{};
    if(::GetFileInformationByHandleEx(fd, FileBasicInfo, &basic, sizeof(basic)) &&
       (basic.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0) {
        return std::nullopt;
    }
    const char* path = nullptr;
    mdb_env_get_path(env, &path);
    auto space = llvm::sys::fs::disk_space(llvm::sys::path::parent_path(path));
    if(space && space->available < extra) {
        return std::format("{} free on its volume, which does not support sparse files",
                           format_bytes(space->available));
    }
    return std::nullopt;
#else
    struct rlimit limit;
    if(getrlimit(RLIMIT_AS, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY) {
        return std::nullopt;
    }
    std::uint64_t used = 0;
#ifdef __linux__
    if(auto status = llvm::MemoryBuffer::getFileAsStream("/proc/self/statm")) {
        llvm::StringRef pages = (*status)->getBuffer().split(' ').first;
        pages.getAsInteger(10, used);
        used *= static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    }
#endif
    if(used + extra > limit.rlim_cur) {
        return std::format("the address space is capped at {} (ulimit -v)",
                           format_bytes(limit.rlim_cur));
    }
    return std::nullopt;
#endif
}

class LmdbDatabase final : public BlobDatabase {
public:
    LmdbDatabase(MDB_env* env, MDB_dbi dbi, MDB_txn* txn, std::string path, bool read_only) :
        env(env), dbi(dbi), txn(txn), path(std::move(path)), read_only_(read_only) {}

    bool read_only() const override {
        return read_only_;
    }

    ~LmdbDatabase() override {
        retire_old_snapshot();
        mdb_txn_abort(txn);
        mdb_env_close(env);
        // Condemned = corruption observed at read time; deleting under the
        // writer lock lets the next open start from an empty database.
        if(condemned) {
            remove_database_files(path);
        }
    }

    ReadBlob read(BlobKind kind, llvm::StringRef key) override {
        auto encoded = encode_key(kind, key);
        auto mk = to_val(encoded);
        MDB_val value;
        if(int rc = mdb_get(txn, dbi, &mk, &value)) {
            note_error(rc);
            return {};
        }
        return blob_of(value);
    }

    bool contains(BlobKind kind, llvm::StringRef key) override {
        auto encoded = encode_key(kind, key);
        auto mk = to_val(encoded);
        MDB_val value;
        // Any error other than "not found" counts as present-but-unreadable;
        // the loader reads corrupted() to decide rebuild vs touch-nothing.
        int rc = mdb_get(txn, dbi, &mk, &value);
        note_error(rc);
        return rc != MDB_NOTFOUND;
    }

    bool write(llvm::ArrayRef<Blob> puts, llvm::ArrayRef<BlobKey> removes) override {
        // The grow request answers the last write alone.
        map_full.store(false, std::memory_order_relaxed);
        auto fail_all = [&](int rc, llvm::StringRef stage) {
            if(rc == MDB_MAP_FULL) {
                map_full.store(true, std::memory_order_relaxed);
            }
            note_error(rc);
            LOG_WARN("Index database write failed ({}): {}", stage, mdb_strerror(rc));
            return false;
        };
        clear_dead_readers(env);
        MDB_txn* wtxn = nullptr;
        if(int rc = mdb_txn_begin(env, nullptr, 0, &wtxn)) {
            return fail_all(rc, "begin");
        }
        for(auto& blob: puts) {
            auto encoded = encode_key(blob.kind, blob.key);
            auto mk = to_val(encoded);
            auto mv = to_val(as_chars(blob.bytes));
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
        if(int rc = cover_declared_pages(env)) {
            LOG_WARN("Cannot extend the index database over its pages: {}", mdb_strerror(rc));
        }
        return true;
    }

    void for_each_key(BlobKind kind, llvm::function_ref<void(llvm::StringRef)> fn) override {
        scan(kind, [&](llvm::StringRef key, const MDB_val&) {
            fn(key);
            return true;
        });
    }

    void for_each(BlobKind kind, llvm::function_ref<bool(llvm::StringRef, ReadBlob)> fn) override {
        scan(kind,
             [&](llvm::StringRef key, const MDB_val& value) { return fn(key, blob_of(value)); });
    }

    std::uint32_t count(BlobKind kind) override {
        std::uint32_t count = 0;
        scan(kind, [&](llvm::StringRef, const MDB_val&) {
            count += 1;
            return true;
        });
        return count;
    }

    std::expected<std::uint64_t, std::string> advance_read_snapshot() override {
        MDB_txn* fresh = nullptr;
        if(int rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &fresh)) {
            note_error(rc);
            return std::unexpected(std::string(mdb_strerror(rc)));
        }
        outstanding.push_back(txn);
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

    Growth grow() override {
        if(!map_full.load(std::memory_order_relaxed)) {
            return Growth::None;
        }
        MDB_envinfo info;
        mdb_env_info(env, &info);
        auto grown = std::max<std::size_t>(info.me_mapsize * 2, lmdb_small_mapsize);
        if(auto refused = growth_refused(env, grown - info.me_mapsize)) {
            LOG_ERROR(
                "The index database cannot grow to {}: {}; the index is not saved until "
                "that changes",
                format_bytes(grown),
                *refused);
            return Growth::Refused;
        }
        // set_mapsize demands no live transaction in this process, so
        // every snapshot dies here; the caller owns rebinding all
        // borrowers before the next suspension point.
        retire_old_snapshot();
        mdb_txn_abort(txn);
        txn = nullptr;
        if(int rc = mdb_env_set_mapsize(env, grown)) {
            LOG_FATAL("Cannot grow the index database to {}: {}",
                      format_bytes(grown),
                      mdb_strerror(rc));
        }
        map_full.store(false, std::memory_order_relaxed);
        if(int rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn)) {
            LOG_FATAL("Cannot read the index database after growing it: {}", mdb_strerror(rc));
        }
        generation += 1;
        return Growth::Grown;
    }

    bool corrupted() const override {
        return poisoned.load(std::memory_order_relaxed);
    }

    void condemn() override {
        condemned = true;
    }

    std::optional<PageUsage> usage() override {
        MDB_envinfo info;
        mdb_env_info(env, &info);
        MDB_stat tree;
        if(mdb_stat(txn, dbi, &tree)) {
            return std::nullopt;
        }
        PageUsage usage{
            .page_size = tree.ms_psize,
            .pages = info.me_last_pgno + 1,
            .tree_pages = tree.ms_branch_pages + tree.ms_leaf_pages,
            .overflow_pages = tree.ms_overflow_pages,
        };
        // LMDB keeps its free list in database 0, one record per write
        // transaction that freed pages: their count, then their numbers.
        MDB_cursor* cursor = nullptr;
        if(mdb_cursor_open(txn, 0, &cursor)) {
            return std::nullopt;
        }
        MDB_val key;
        MDB_val value;
        while(mdb_cursor_get(cursor, &key, &value, MDB_NEXT) == 0) {
            std::size_t count = 0;
            std::memcpy(&count, value.mv_data, sizeof(count));
            usage.free_pages += count;
        }
        mdb_cursor_close(cursor);
        return usage;
    }

private:
    /// A value of the read snapshot as a ReadBlob. LMDB keeps bytes
    /// readable_in_place only for overflow-page values; a small inline
    /// value is copied and the copy is immortal (generation 0).
    ReadBlob blob_of(const MDB_val& value) const {
        llvm::StringRef bytes(static_cast<const char*>(value.mv_data), value.mv_size);
        if(!readable_in_place(bytes)) {
            return {.buffer = llvm::MemoryBuffer::getMemBufferCopy(bytes)};
        }
        return {.buffer = llvm::MemoryBuffer::getMemBuffer(bytes,
                                                           "",
                                                           /*RequiresNullTerminator=*/false),
                .generation = generation};
    }

    /// Visit the records of `kind` in key order, keys without their prefix,
    /// until `fn` returns false.
    void scan(BlobKind kind,
              llvm::function_ref<bool(llvm::StringRef key, const MDB_val& value)> fn) {
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
            if(bytes.empty() || bytes[0] != prefix || !fn(bytes.drop_front(), value)) {
                break;
            }
            rc = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
        }
        if(rc != 0 && rc != MDB_NOTFOUND) {
            note_error(rc);
        }
        mdb_cursor_close(cursor);
    }

    /// Latch corruption-family read errors for the loader's rebuild
    /// decision; everything else stays "missing or transiently unreadable".
    void note_error(int rc) {
        if(is_corruption(rc)) {
            poisoned.store(true, std::memory_order_relaxed);
        }
    }

private:
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

    bool read_only_ = false;
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
    // writing our meta into them would bypass the schema gate.
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

std::unique_ptr<LmdbDatabase> open_lmdb_env(llvm::StringRef directory,
                                            std::size_t initial_mapsize,
                                            bool read_only) {
    auto path = path::join(directory, lmdb_file_name);

    // LMDB takes an empty file for a new database and writes its first
    // pages, which a read-only open cannot.
    if(read_only) {
        if(auto status = vfs::status(path); status && status->stamp.size == 0) {
            LOG_WARN("Index database at {} is empty; run `clice index` to repair it", path);
            return nullptr;
        }
    }

    auto mapsize = initial_mapsize != 0 ? initial_mapsize : default_mapsize();

    // One recovery retry: confirmed corruption (or a meta mismatch) is
    // repaired by deleting the database — it is a rebuildable cache, and
    // the writer lock guarantees no other live process is using it.
    // Transient errors (permissions, fd/memory pressure) must NOT delete
    // anything: persistence is disabled for this session instead, the
    // same discipline the loader applies to an unreadable header.
    bool repaired = false;
    int reopens = 0;
    while(true) {
        // Giving up must not leave behind a file this attempt created
        // (make_sparse and mdb_env_open both create on demand): a later
        // session would misread the abandoned uninitialized placeholder as
        // corruption and log a spurious rebuild.
        bool created = !read_only && !vfs::exists(path);
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
        // Every `clice query` beside a server holds a slot while it runs;
        // LMDB's default of 126 is within reach of an agent's parallel calls.
        mdb_env_set_maxreaders(env, 1024);
        unsigned flags = MDB_NOSUBDIR | MDB_NOTLS | (read_only ? MDB_RDONLY : 0);
        // True = the failure was repaired by deleting the database and the
        // loop should retry; false = give up with persistence disabled.
        auto fail = [&](int rc, llvm::StringRef stage) {
            mdb_env_close(env);
            if(read_only && is_corruption(rc)) {
                LOG_WARN(
                    "Index database at {} is damaged ({} failed: {}); run `clice index` to "
                    "repair it",
                    path,
                    stage,
                    mdb_strerror(rc));
                return false;
            }
            if(is_corruption(rc) && !repaired) {
                LOG_WARN("Index database at {} is corrupt ({} failed: {}); rebuilding",
                         path,
                         stage,
                         mdb_strerror(rc));
                remove_database_files(path);
                repaired = true;
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
            clear_dead_readers(env);
        }
        MDB_txn* txn = nullptr;
        int rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn);
        if(rc == MDB_MAP_RESIZED) {
            // Another process grew the map between open and here; adopt
            // its size (no transaction is active yet) and retry once.
            mdb_env_set_mapsize(env, 0);
            rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn);
        }
        // The last process to close finds itself alone and destroys the
        // lock file's mutexes, racing an opener blocked on its shared lock
        // meanwhile: that opener's first transaction fails with EINVAL. A
        // reopen finds the lock file unowned and initializes it afresh.
        if(rc == EINVAL && reopens < 3) {
            mdb_env_close(env);
            reopens += 1;
            // No event: the closing process is done with the lock file
            // unannounced; each reopen gives it a little longer.
            std::this_thread::sleep_for(std::chrono::milliseconds(10 * reopens));
            continue;
        }
        if(rc != 0) {
            if(fail(rc, "snapshot")) {
                continue;
            }
            return nullptr;
        }
        if(read_only) {
            if(auto [length, declared] = coverage(env); length < declared) {
                mdb_txn_abort(txn);
                mdb_env_close(env);
                LOG_WARN(
                    "Index database at {} is shorter than its pages ({} of {}); run "
                    "`clice index` to repair it",
                    path,
                    format_bytes(length),
                    format_bytes(declared));
                return nullptr;
            }
        } else if(int cover_rc = cover_declared_pages(env)) {
            mdb_txn_abort(txn);
            if(fail(cover_rc, "cover")) {
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
        return std::make_unique<LmdbDatabase>(env, dbi, txn, std::move(path), read_only);
    }
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

std::string database_directory(const CacheStore& store, llvm::StringRef configuration) {
    std::string name = "default";
    if(!configuration.empty()) {
        name = configuration.take_front(32).lower();
        for(char& c: name) {
            if(!llvm::isAlnum(c) && c != '-' && c != '_') {
                c = '_';
            }
        }
        name += std::format("~{:016x}", llvm::xxh3_64bits(configuration));
    }
    return path::join(store.base_dir(), "index", name);
}

bool database_exists(const CacheStore& store, llvm::StringRef configuration) {
    return vfs::exists(path::join(database_directory(store, configuration), lmdb_file_name));
}

std::unique_ptr<BlobDatabase> open_lmdb_database(CacheStore& store,
                                                 llvm::StringRef configuration,
                                                 std::size_t initial_mapsize,
                                                 bool read_only) {
    read_only = read_only || store.read_only();
    auto directory = database_directory(store, configuration);
    if(read_only) {
        if(!database_exists(store, configuration)) {
            return nullptr;
        }
    } else if(auto ec = vfs::create_directories(directory)) {
        LOG_WARN("Cannot create the index database directory {}: {}", directory, ec.message());
        return nullptr;
    }
    auto db = open_lmdb_env(directory, initial_mapsize, read_only);
    if(!db) {
        return nullptr;
    }
    LOG_INFO("Index database: {}", directory);
    return db;
}

std::unique_ptr<BlobDatabase> open_database(CacheStore& store,
                                            llvm::StringRef configuration,
                                            bool read_only) {
    // FIXME: no index persistence on remote filesystems. A per-file blob
    // backend used to fill this gap (one CacheStore-namespace file per
    // blob, removed in PR #650 — see its history to resurrect it), but it
    // duplicated everything the database gives for free — atomic batches,
    // read snapshots, corruption detection — while the remote scenarios
    // it was meant to serve (NFS home directories, SMB project shares,
    // WSL drvfs checkouts) differ enough in locking and cache-coherence
    // behavior that one untested fallback cannot honestly cover them.
    // What remote workspaces actually need is an open design question;
    // until it is answered, such sessions run with an in-memory index
    // and this warning.
    switch(filesystem_locality(store.base_dir())) {
        case FsLocality::Local: break;
        case FsLocality::Remote: {
            LOG_WARN(
                "{} is on a remote filesystem, which the index database does not "
                "support; index persistence is disabled for this session",
                store.base_dir());
            return nullptr;
        }
        case FsLocality::Unknown: {
            LOG_WARN(
                "{} is on a FUSE filesystem; the index database needs "
                "local-filesystem semantics and may misbehave there",
                store.base_dir());
            break;
        }
    }
    return open_lmdb_database(store, configuration, 0, read_only);
}

}  // namespace clice::index
