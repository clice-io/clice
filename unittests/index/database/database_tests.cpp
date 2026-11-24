module;

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

module clice;

import :index.database;
import :support.text;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.cache_store;
import :vfs.path;

namespace clice::testing {
namespace {

constexpr std::uint32_t version = 1;

CacheStore open_store(TempDir& tmp, llvm::StringRef sub, bool read_only = false) {
    auto store = CacheStore::open(tmp.path(sub), version, read_only);
    ZASSERT(store.has_value());
    return std::move(*store);
}

index::BlobDatabase::Blob blob(index::BlobKind kind, llvm::StringRef key, llvm::StringRef bytes) {
    return {kind, key.str(), copy_bytes(bytes)};
}

/// Value sizes on the two LMDB layouts: large lands on overflow pages
/// (16-aligned, served borrowed), small stays inline in the leaf
/// (2-aligned at worst, served as an owned copy).
std::string large_value(char fill) {
    return std::string(8192, fill);
}

/// The `-open` file of a database locked shared, as another process
/// reading the database holds it.
struct ForeignReader {
    explicit ForeignReader(const std::string& database) {
        auto opened =
            fs::File::open(database + "-open", {.read = true, .write = true, .create = true});
        ZASSERT(opened.has_value());
        file = std::move(*opened);
        ZASSERT(file.try_lock_shared().value_or(false));
    }

    // Windows frees a lock its handle's close leaves at some later time.
    ~ForeignReader() {
        file.unlock();
    }

    fs::File file;
};

/// The database open in a process outside the `-open` file's protocol, as
/// an older clice holds it. On POSIX a child process takes LMDB's record
/// lock on the `-lock` file, which never conflicts with its own process,
/// and opening the database in it would follow a fork LMDB does not
/// survive. Windows locks belong to the handle, so a second environment
/// stands in, holding the lock and the mutexes LMDB names in the `-lock`
/// file.
struct OlderOpener {
    explicit OlderOpener(const std::string& database) {
#ifdef _WIN32
        ZASSERT(mdb_env_create(&env) == 0);
        ZASSERT(mdb_env_open(env, database.c_str(), MDB_NOSUBDIR | MDB_NOTLS | MDB_RDONLY, 0644) ==
                0);
        ZASSERT(mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn) == 0);
#else
        auto lock_file = database + "-lock";
        int ready[2];
        int done[2];
        ZASSERT(::pipe(ready) == 0);
        ZASSERT(::pipe(done) == 0);
        child = ::fork();
        ZASSERT(child >= 0);
        if(child == 0) {
            // The parent's close of `done` reaches no reader while this
            // process holds its write end too.
            ::close(done[1]);
            ::close(ready[0]);
            int lock_fd = ::open(lock_file.c_str(), O_RDWR);
            struct flock lock{};
            lock.l_type = F_RDLCK;
            lock.l_whence = SEEK_SET;
            lock.l_len = 1;
            char locked = lock_fd >= 0 && ::fcntl(lock_fd, F_SETLK, &lock) == 0;
            [[maybe_unused]] auto written = ::write(ready[1], &locked, 1);
            [[maybe_unused]] auto read = ::read(done[0], &locked, 1);
            ::_exit(0);
        }
        ::close(ready[1]);
        ::close(done[0]);
        char locked = 0;
        ZASSERT(::read(ready[0], &locked, 1) == 1);
        ZASSERT(locked);
        ::close(ready[0]);
        release = done[1];
#endif
    }

    ~OlderOpener() {
#ifdef _WIN32
        mdb_txn_abort(txn);
        mdb_env_close(env);
#else
        ::close(release);
        ::waitpid(child, nullptr, 0);
#endif
    }

#ifdef _WIN32
    MDB_env* env = nullptr;

    MDB_txn* txn = nullptr;
#else
    pid_t child = -1;

    /// Closed to let the child exit.
    int release = -1;
#endif
};

/// A database of 64 values on overflow pages, 56 of them removed again, so
/// most of its pages are free.
std::unique_ptr<index::BlobDatabase> sparse_database(CacheStore& store) {
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    std::vector<index::BlobDatabase::Blob> blobs;
    llvm::SmallVector<index::BlobKey> removals;
    for(std::uint32_t i = 0; i < 64; i += 1) {
        blobs.push_back(
            blob(index::BlobKind::Shard, std::to_string(i), std::string(1 << 16, 'a' + i % 26)));
        if(i < 56) {
            removals.push_back({index::BlobKind::Shard, std::to_string(i)});
        }
    }
    ZASSERT(db->write(blobs, {}).has_value());
    ZASSERT(db->write({}, removals).has_value());
    ZASSERT(db->advance_read_snapshot().has_value());
    db->retire_old_snapshot();
    return db;
}

/// What a compact copy gets for its `abandon`: nothing gives it up.
const std::atomic<bool> keep = false;

ZEST_SUITE(IndexDatabase) {

ZEST_CASE(WriteReadRoundTrip) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);

    ZASSERT(db->write({blob(index::BlobKind::Shard, "a", large_value('a')),
                       blob(index::BlobKind::Header, "header", "gg")},
                      {}));
    // Reads serve the resident snapshot; committed writes become
    // visible only after an advance.
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();

    auto shard = db->read(index::BlobKind::Shard, "a");
    ZASSERT(bool(shard));
    ZASSERT(shard.buffer->getBuffer() == large_value('a'));
    ZASSERT(db->contains(index::BlobKind::Header, "header"));
    ZASSERT(!db->contains(index::BlobKind::Shard, "missing"));
    ZASSERT(!bool(db->read(index::BlobKind::Shard, "missing")));
}

ZEST_CASE(WriteRemoves) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);

    ZASSERT(db->write({blob(index::BlobKind::TUManifest, "m1", "one"),
                       blob(index::BlobKind::TUManifest, "m2", "two")},
                      {}));
    ZASSERT(db->write(
        {
    },
        {{index::BlobKind::TUManifest, "m1"}}));
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();

    ZASSERT(!db->contains(index::BlobKind::TUManifest, "m1"));
    ZASSERT(db->contains(index::BlobKind::TUManifest, "m2"));
}

ZEST_CASE(KindsAreIsolated) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);

    ZASSERT(db->write({blob(index::BlobKind::Shard, "same", "shard"),
                       blob(index::BlobKind::TUManifest, "same", "manifest")},
                      {}));
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();

    llvm::SmallVector<std::string> shard_keys;
    db->for_each_key(index::BlobKind::Shard,
                     [&](llvm::StringRef key) { shard_keys.push_back(key.str()); });
    ZASSERT(shard_keys.size() == 1);
    ZASSERT(shard_keys.front() == "same");
    ZASSERT(db->read(index::BlobKind::TUManifest, "same").buffer->getBuffer() == "manifest");
    ZASSERT(!db->contains(index::BlobKind::Header, "same"));
}

ZEST_CASE(SnapshotPinsUntilAdvance) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);

    ZASSERT(db->write({blob(index::BlobKind::Shard, "k", large_value('1'))}, {}));
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();

    auto before = db->read(index::BlobKind::Shard, "k");
    ZASSERT(bool(before));

    // Committed writes stay invisible to the resident snapshot until the
    // next advance; the borrowed buffer keeps serving the old bytes.
    ZASSERT(db->write({blob(index::BlobKind::Shard, "k", large_value('2'))}, {}));
    ZASSERT(db->read(index::BlobKind::Shard, "k").buffer->getBuffer() == large_value('1'));
    ZASSERT(before.buffer->getBuffer() == large_value('1'));

    auto advanced = db->advance_read_snapshot();
    ZASSERT(advanced);
    ZASSERT(*advanced != 0);
    // Old and new snapshots serve their own bytes side by side until the
    // old one retires — the migration window's core invariant.
    ZASSERT(db->read(index::BlobKind::Shard, "k").buffer->getBuffer() == large_value('2'));
    ZASSERT(before.buffer->getBuffer() == large_value('1'));
    db->retire_old_snapshot();
}

ZEST_CASE(SmallValuesCopiedAligned) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);

    ZASSERT(db->write({blob(index::BlobKind::TUManifest, "small", "tiny"),
                       blob(index::BlobKind::Shard, "big", large_value('b'))},
                      {}));
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();

    // Inline leaf values are only 2-aligned, so they must come back as an
    // owned copy (generation 0); overflow-page values are borrowed.
    auto small = db->read(index::BlobKind::TUManifest, "small");
    ZASSERT(bool(small));
    ZASSERT(small.generation == 0);
    ZASSERT(reinterpret_cast<std::uintptr_t>(small.buffer->getBufferStart()) % 8 == 0);

    auto big = db->read(index::BlobKind::Shard, "big");
    ZASSERT(bool(big));
    ZASSERT(big.generation != 0);
    ZASSERT(reinterpret_cast<std::uintptr_t>(big.buffer->getBufferStart()) % 8 == 0);
}

ZEST_CASE(ReopenServesPersistedBlobs) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    {
        auto db = index::open_database(store, {}).value_or(nullptr);
        ZASSERT(db != nullptr);
        ZASSERT(db->write({blob(index::BlobKind::Members, "cdb", "snapshot")}, {}));
    }
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    ZASSERT(db->read(index::BlobKind::Members, "cdb").buffer->getBuffer() == "snapshot");
}

ZEST_CASE(CorruptDatabaseRebuilds) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    {
        auto directory = index::database_directory(store, {});
        ZASSERT(fs::create_directories(directory).has_value());
        ZASSERT(
            !fs::write_file(path::join(directory, "index.mdb"),
                            "this is not an lmdb file, not even close, but long enough to map"));
    }
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    ZASSERT(!db->contains(index::BlobKind::Members, "cdb"));
    ZASSERT(db->write({blob(index::BlobKind::Members, "cdb", "fresh")}, {}));
}

/// A database of `count` large blobs written over several commits, so
/// its pages run well past the two meta pages; returns the file's path.
std::string populated_database(CacheStore& store, int count) {
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    for(int i = 0; i < count; i += 1) {
        ZASSERT(db->write({blob(index::BlobKind::Shard, std::to_string(i), large_value('t'))}, {})
                    .has_value());
    }
    return path::join(index::database_directory(store, {}), "index.mdb");
}

/// Cuts the file down to its two meta pages: the tree they point at is gone.
void truncate_to_meta(llvm::StringRef file) {
    auto opened = fs::File::open(file, {.read = true, .write = true});
    ZASSERT(opened.has_value());
    ZASSERT(!opened->resize(2 * sys::page_size()));
}

/// Rewrite the record describing the database, keyed "\xffmeta", with
/// the schema another version of clice writes.
void write_foreign_meta(llvm::StringRef file) {
    MDB_env* env = nullptr;
    ZASSERT(mdb_env_create(&env) == 0);
    ZASSERT(mdb_env_open(env, file.str().c_str(), MDB_NOSUBDIR, 0644) == 0);
    MDB_txn* txn = nullptr;
    ZASSERT(mdb_txn_begin(env, nullptr, 0, &txn) == 0);
    MDB_dbi dbi;
    ZASSERT(mdb_dbi_open(txn, nullptr, 0, &dbi) == 0);
    std::uint32_t meta[] = {1, sizeof(std::size_t) * 8, 0x01020304};
    MDB_val key{5, const_cast<char*>("\xffmeta")};
    MDB_val value{sizeof(meta), meta};
    ZASSERT(mdb_put(txn, dbi, &key, &value, 0) == 0);
    ZASSERT(mdb_txn_commit(txn) == 0);
    mdb_env_close(env);
}

ZEST_CASE(ForeignDatabaseRebuilds) {
    TempDir tmp;
    std::string file;
    {
        auto store = open_store(tmp, "lmdb");
        file = populated_database(store, 1);
    }
    write_foreign_meta(file);
    {
        auto store = open_store(tmp, "lmdb", /*read_only=*/true);
        ZEXPECT(!index::open_database(store, {}));
    }
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    ZEXPECT(!db->contains(index::BlobKind::Shard, "0"));
    ZEXPECT(!db->corrupted());
}

ZEST_CASE(TruncatedDatabaseRebuilds) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    truncate_to_meta(populated_database(store, 8));

    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    ZASSERT(!db->contains(index::BlobKind::Shard, "7"));
    ZASSERT(db->write({blob(index::BlobKind::Members, "cdb", "fresh")}, {}));
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();
    ZASSERT(db->read(index::BlobKind::Members, "cdb").buffer->getBuffer() == "fresh");
}

/// A database of `count` small records of three kinds, committed at once:
/// each kind spans several leaf pages, the paths' between the files' and
/// the symbols'.
std::string small_records_database(CacheStore& store, int count) {
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    std::vector<index::BlobDatabase::Blob> blobs;
    for(int i = 0; i < count; i += 1) {
        auto key = std::format("{:05}", i);
        blobs.push_back(blob(index::BlobKind::File, key, std::string(40, 'f')));
        blobs.push_back(blob(index::BlobKind::Path, key, std::string(40, 'p')));
        blobs.push_back(blob(index::BlobKind::Symbol, key, std::string(40, 'y')));
    }
    ZASSERT(db->write(blobs, {}).has_value());
    return path::join(index::database_directory(store, {}), "index.mdb");
}

/// Swap the first two keys of the second leaf page holding nothing but
/// keys starting with `prefix`, so a walk reaches the page from the one
/// before and reads its keys out of order. An LMDB page starts with a
/// 16-byte header (flags at 10, the end of the node pointers at 12), node
/// pointers follow it, and a node is four 16-bit fields, the last its key
/// size, then the key.
void swap_page_keys(llvm::StringRef file, char prefix) {
    std::string bytes;
    ZASSERT(!fs::read_file(bytes, file));
    auto page_size = static_cast<std::uint32_t>(sys::page_size());
    auto u16 = [&](std::size_t at) {
        std::uint16_t value = 0;
        std::memcpy(&value, bytes.data() + at, sizeof(value));
        return value;
    };
    std::vector<std::pair<std::string, std::size_t>> pages;
    for(std::size_t page = 2 * page_size; page + page_size <= bytes.size(); page += page_size) {
        constexpr std::uint16_t leaf = 0x02;
        if(u16(page + 10) != leaf || u16(page + 12) <= 18) {
            continue;
        }
        bool only_prefix = true;
        for(std::size_t pointer = page + 16; pointer < page + u16(page + 12); pointer += 2) {
            auto node = page + u16(pointer);
            only_prefix = only_prefix && u16(node + 6) > 0 && bytes[node + 8] == prefix;
        }
        if(only_prefix) {
            auto node = page + u16(page + 16);
            pages.emplace_back(bytes.substr(node + 8, u16(node + 6)), page);
        }
    }
    ranges::sort(pages);
    ZASSERT(pages.size() > 1);
    auto pointers = pages[1].second + 16;
    auto first = bytes.begin() + static_cast<std::ptrdiff_t>(pointers);
    ranges::swap_ranges(first, first + 2, first + 2, first + 4);
    ZASSERT(!fs::write_file(file, bytes));
}

ZEST_CASE(ScanMeetsDamage) {
    TempDir tmp;
    std::string file;
    {
        auto store = open_store(tmp, "lmdb");
        file = small_records_database(store, 2000);
    }
    swap_page_keys(file, 'Y');
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    ZEXPECT(db->count(index::BlobKind::File) == 2000u);
    ZEXPECT(!db->corrupted());
    db->count(index::BlobKind::Symbol);
    ZEXPECT(db->corrupted());
}

ZEST_CASE(ReadOnlyRefusesTruncated) {
    TempDir tmp;
    {
        auto store = open_store(tmp, "ws");
        truncate_to_meta(populated_database(store, 8));
    }
    auto store = open_store(tmp, "ws", /*read_only=*/true);
    ZASSERT(!index::open_database(store, {}));
}

ZEST_CASE(ReadOnlyRefusesEmpty) {
    TempDir tmp;
    std::string file;
    {
        auto store = open_store(tmp, "ws");
        file = populated_database(store, 1);
    }
    ZASSERT(!fs::write_file(file, ""));
    auto store = open_store(tmp, "ws", /*read_only=*/true);
    ZASSERT(index::database_exists(store, {}));
    ZASSERT(!index::open_database(store, {}));
    auto status = fs::status(file);
    ZASSERT(status);
    ZASSERT(status->size == 0U);
}

ZEST_CASE(WritesCoverFreedTail) {
    // A batch whose freed pages LMDB hands back to its free list is never
    // written, and when they sit at the end the file stays shorter than
    // the pages its meta declares — healthy, yet what a truncation looks
    // like to a read-only opener. Every commit must cover them.
    TempDir tmp;
    std::string file;
    {
        auto store = open_store(tmp, "lmdb");
        file = path::join(index::database_directory(store, {}), "index.mdb");
        auto db = index::open_database(store, {}).value_or(nullptr);
        ZASSERT(db != nullptr);
        auto advance_snapshot = [&] {
            ZASSERT(db->advance_read_snapshot());
            db->retire_old_snapshot();
        };
        ZASSERT(db->write({blob(index::BlobKind::Shard, "x", large_value('x'))}, {}));
        advance_snapshot();
        ZASSERT(db->write(
            {
        },
            {{index::BlobKind::Shard, "x"}}));
        advance_snapshot();
        ZASSERT(db->write(
            {
                blob(index::BlobKind::Members, "cdb", "small"),
                blob(index::BlobKind::Shard, "h", std::string(1 << 16, 'h'))
        },
            {{index::BlobKind::Shard, "h"}}));
    }

    MDB_env* env = nullptr;
    ZASSERT(mdb_env_create(&env) == 0);
    ZASSERT(mdb_env_open(env, file.c_str(), MDB_NOSUBDIR | MDB_RDONLY, 0644) == 0);
    MDB_envinfo info;
    mdb_env_info(env, &info);
    MDB_stat db_stat;
    mdb_env_stat(env, &db_stat);
    mdb_env_close(env);
    auto status = fs::status(file);
    ZASSERT(status);
    ZEXPECT(status->size >= (info.me_last_pgno + 1) * db_stat.ms_psize);

    auto store = open_store(tmp, "lmdb", /*read_only=*/true);
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    ZASSERT(db->contains(index::BlobKind::Members, "cdb"));
}

ZEST_CASE(DefaultOpenFileBounded) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    ZASSERT(db->write({blob(index::BlobKind::Members, "cdb", "x")}, {}));

    // On Windows the mapping extends index.mdb to the whole mapsize, so
    // this pins the small default (a 64 GiB logical file is the bug the
    // default exists to avoid); POSIX file sizes track the data
    // high-water mark and pass trivially.
    auto status = fs::status(path::join(index::database_directory(store, {}), "index.mdb"));
    ZASSERT(status);
    ZASSERT((status->size <= 256ull << 20));
}

ZEST_CASE(OverflowFailsThenGrows) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    // Small enough that a handful of large values exhausts it.
    auto db = index::open_lmdb_database(store, {}, 256 * 1024).value_or(nullptr);
    ZASSERT(db != nullptr);

    std::vector<index::BlobDatabase::Blob> puts;
    for(int i = 0; i < 64; i += 1) {
        puts.push_back(blob(index::BlobKind::Shard, std::to_string(i), large_value('x')));
    }
    ZASSERT(!db->write(puts, {}));
    ZASSERT(!db->contains(index::BlobKind::Shard, "0"));

    ZASSERT(db->grow() == index::BlobDatabase::Growth::Grown);
    // grow() opened a fresh snapshot, so this proves the failed batch
    // really committed nothing (the pre-grow check only saw the pinned
    // old snapshot).
    ZASSERT(!db->contains(index::BlobKind::Shard, "0"));
    // A second grow without a latched full map is a no-op.
    ZASSERT(db->grow() == index::BlobDatabase::Growth::None);

    ZASSERT(db->write(puts, {}));
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();
    ZASSERT(db->read(index::BlobKind::Shard, "63").buffer->getBuffer() == large_value('x'));
}

ZEST_CASE(ReadOnlyMissingDatabase) {
    TempDir tmp;
    { auto store = open_store(tmp, "empty"); }
    auto store = open_store(tmp, "empty", /*read_only=*/true);
    // A reader before any writer ran has nothing to read: persistence is
    // disabled rather than creating an index.mdb a read-only session must
    // not leave behind.
    auto db = index::open_database(store, {}, /*read_only=*/true).value_or(nullptr);
    ZASSERT(db == nullptr);
    ZASSERT(!fs::exists(index::database_directory(store, {})));
}

ZEST_CASE(ReadOnlyServesExisting) {
    TempDir tmp;
    {
        auto store = open_store(tmp, "ws");
        auto db = index::open_database(store, {}).value_or(nullptr);
        ZASSERT(db != nullptr);
        ZASSERT(db->write({blob(index::BlobKind::Header, "header", "gg")}, {}));
    }
    auto store = open_store(tmp, "ws", /*read_only=*/true);
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    ZASSERT(db->contains(index::BlobKind::Header, "header"));
    ZASSERT(db->read(index::BlobKind::Header, "header").buffer->getBuffer() == "gg");
}

ZEST_CASE(CondemnedDeletesOnClose) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    {
        auto db = index::open_database(store, {}).value_or(nullptr);
        ZASSERT(db != nullptr);
        ZASSERT(db->write({blob(index::BlobKind::Members, "cdb", "bytes")}, {}));
        db->condemn();
    }
    ZASSERT(!fs::exists(path::join(index::database_directory(store, {}), "index.mdb")));
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    ZASSERT(!db->contains(index::BlobKind::Members, "cdb"));
}

ZEST_CASE(DatabasePerConfiguration) {
    /// Every configuration has a database of its own under `index/`, the
    /// anonymous one named `default`; what one holds the other never sees.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    ZEXPECT(index::database_directory(store, {}) ==
            path::join(store.base_dir(), "index", "default"));
    auto directory = index::database_directory(store, {"release"});
    ZEXPECT(path::parent(directory) == path::join(store.base_dir(), "index"));
    ZEXPECT(path::filename(directory).starts_with("release~"));

    auto debug = index::open_database(store, {"debug"}).value_or(nullptr);
    auto release = index::open_database(store, {"release"}).value_or(nullptr);
    ZASSERT(debug != nullptr);
    ZASSERT(release != nullptr);
    ZASSERT(debug->write({blob(index::BlobKind::Header, "header", "d")}, {}));
    ZASSERT(release->advance_read_snapshot());
    ZASSERT(!release->contains(index::BlobKind::Header, "header"));
    ZASSERT(fs::exists(path::join(index::database_directory(store, {"debug"}), "index.mdb")));
    ZASSERT(fs::exists(path::join(index::database_directory(store, {"release"}), "index.mdb")));
}

ZEST_CASE(DatabaseNameSanitized) {
    /// The name's prefix is the tag reduced to a safe lowercase spelling;
    /// the hash behind the `~` tells apart tags that reduce alike, alias
    /// each other on a case-folding filesystem, or spell `default`.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto name = [&](llvm::StringRef configuration) {
        return std::string(path::filename(index::database_directory(store, {configuration.str()})));
    };
    auto prefix = [&](llvm::StringRef configuration) {
        auto full = name(configuration);
        return llvm::StringRef(full).rsplit('~').first.str();
    };
    ZEXPECT(prefix("Debug-x86_64.v2") == "debug-x86_64_v2");
    ZEXPECT(name("Debug") != name("debug"));
    ZEXPECT(prefix("linux/arm") == "linux_arm");
    ZEXPECT(name("linux/arm") != name("linux\\arm"));
    ZEXPECT(name("linux/arm") != name("linux arm"));
    ZEXPECT(prefix("a~b") == "a_b");
    ZEXPECT(prefix("release.") == "release_");
    ZEXPECT(prefix("..") == "__");
    ZEXPECT(name("default") != "default");
    ZEXPECT(prefix(std::string(40, 'x')) == std::string(32, 'x'));
}

ZEST_CASE(DatabasePerResolvedBuild) {
    /// A build resolved through links has a database apart from its
    /// configuration's own and from every other build, named by the tag and
    /// the trailing directories of its last database, as many whole ones as
    /// fit.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto name = [&](const index::DatabaseKey& key) {
        return std::string(path::filename(index::database_directory(store, key)));
    };
    auto prefix = [&](const index::DatabaseKey& key) {
        auto full = name(key);
        return llvm::StringRef(full).rsplit('~').first.str();
    };
    index::DatabaseKey asan{"active",
                            "/w/out/variants/linux/x86_64/debug/asan/compile_commands.json"};
    index::DatabaseKey none{"active",
                            "/w/out/variants/linux/x86_64/release/none/compile_commands.json"};
    ZEXPECT(prefix(asan) == "active-linux_x86_64_debug_asan");
    ZEXPECT(name(asan) != name(none));
    ZEXPECT(name(asan) != name({"active"}));
    ZEXPECT(prefix({"", "/w/build/compile_commands.json"}) == "w_build");
    ZEXPECT(name({"", "/w/build/compile_commands.json"}) != name({}));
    ZEXPECT(prefix({"active", "/w/a/compile_commands.json\n" + asan.resolved}) ==
            "active-linux_x86_64_debug_asan");
    ZEXPECT(prefix({"active", "/w/" + std::string(40, 'y') + "/compile_commands.json"}) ==
            "active-");
}

ZEST_CASE(DatabaseBlockedByFile) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto directory = index::database_directory(store, {"x"});
    ZASSERT(fs::create_directories(path::parent(directory)));
    ZASSERT(!fs::write_file(directory, "x"));
    ZASSERT(!index::open_database(store, {"x"}));
}

ZEST_CASE(OutstandingSnapshotsStack) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);

    ZASSERT(db->write({blob(index::BlobKind::Shard, "k", large_value('1'))}, {}));
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();
    auto lease = db->read(index::BlobKind::Shard, "k");
    ZASSERT(bool(lease));

    // A cancelled migration leaves its old snapshot outstanding and the
    // next advance stacks another; every stacked snapshot keeps its
    // borrowers alive until one retire clears them all.
    ZASSERT(db->write({blob(index::BlobKind::Shard, "k", large_value('2'))}, {}));
    ZASSERT(db->advance_read_snapshot());
    ZASSERT(db->write({blob(index::BlobKind::Shard, "k", large_value('3'))}, {}));
    ZASSERT(db->advance_read_snapshot());
    ZASSERT(lease.buffer->getBuffer() == large_value('1'));
    ZASSERT(db->read(index::BlobKind::Shard, "k").buffer->getBuffer() == large_value('3'));
    db->retire_old_snapshot();
    ZASSERT(db->read(index::BlobKind::Shard, "k").buffer->getBuffer() == large_value('3'));
}

ZEST_CASE(CompactCopyFillsPages) {
    // Small records written in random order leave their pages part empty;
    // the copy lays them out in key order on full pages.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    std::mt19937 random(7);
    for(std::uint32_t round = 0; round < 20; round += 1) {
        std::vector<index::BlobDatabase::Blob> blobs;
        for(std::uint32_t i = 0; i < 1000; i += 1) {
            blobs.push_back(blob(index::BlobKind::Symbol,
                                 index::number_key(std::uint64_t(random())),
                                 std::string(80, 'a' + i % 26)));
        }
        ZASSERT(db->write(blobs, {}).has_value());
    }
    ZASSERT(db->advance_read_snapshot().has_value());
    db->retire_old_snapshot();
    auto before = db->usage();
    ZASSERT(before);
    ZASSERT(db->write_compact_copy(keep) == index::BlobDatabase::Copy::Written);
    ZASSERT(db->swap_compact_copy() == index::BlobDatabase::Swap::Replaced);
    auto after = db->usage();
    ZASSERT(after);
    ZEXPECT(after->tree_pages * 10 < before->tree_pages * 8);
    ZEXPECT(db->count(index::BlobKind::Symbol) == 20000u);
}

ZEST_CASE(AbandonedCopyLeavesNothing) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    auto db = sparse_database(store);
    const std::atomic<bool> abandon = true;
    ZEXPECT(db->write_compact_copy(abandon) == index::BlobDatabase::Copy::Abandoned);
    ZEXPECT(!fs::exists(file + "-compact"));
    auto value = db->read(index::BlobKind::Shard, "60");
    ZASSERT(bool(value));
    ZEXPECT(value.buffer->getBuffer() == std::string(1 << 16, 'a' + 60 % 26));
}

ZEST_CASE(CompactCopyShrinksFile) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    auto db = sparse_database(store);
    auto before = db->usage();
    ZASSERT(before);
    ZASSERT(before->free_pages > before->pages / 2);

    ZASSERT(db->write_compact_copy(keep) == index::BlobDatabase::Copy::Written);
    ZASSERT(db->swap_compact_copy() == index::BlobDatabase::Swap::Replaced);
    auto after = db->usage();
    ZASSERT(after);
    ZEXPECT(after->pages * 2 < before->pages);
    ZEXPECT(after->free_pages == 0u);
    ZEXPECT(!fs::exists(file + "-compact"));
    for(std::uint32_t i = 56; i < 64; i += 1) {
        auto value = db->read(index::BlobKind::Shard, std::to_string(i));
        ZASSERT(bool(value));
        ZEXPECT(value.buffer->getBuffer() == std::string(1 << 16, 'a' + i % 26));
    }
    ZEXPECT(!db->contains(index::BlobKind::Shard, "0"));

    // The reopened handle writes on, and the next open reads the copy.
    ZASSERT(db->write({blob(index::BlobKind::Header, "header", "gg")}, {}));
    db.reset();
    auto again = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(again != nullptr);
    ZEXPECT(again->contains(index::BlobKind::Header, "header"));
    ZEXPECT(again->contains(index::BlobKind::Shard, "63"));
}

ZEST_CASE(ReaderKeepsFile) {
    // Another process holding the database open keeps its file, whether it
    // was there before the copy or opened the database while it was written:
    // the handle serves on as before.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    auto db = sparse_database(store);
    auto pages = db->usage()->pages;
    auto value = db->read(index::BlobKind::Shard, "60");
    ZASSERT(bool(value));
    {
        ForeignReader reader(file);
        ZEXPECT(db->write_compact_copy(keep) == index::BlobDatabase::Copy::Busy);
        ZEXPECT(!fs::exists(file + "-compact"));
    }
    ZASSERT(db->write_compact_copy(keep) == index::BlobDatabase::Copy::Written);
    {
        ForeignReader reader(file);
        ZEXPECT(db->swap_compact_copy() == index::BlobDatabase::Swap::Busy);
        ZEXPECT(!fs::exists(file + "-compact"));
        ZEXPECT(db->usage()->pages == pages);
        ZEXPECT(value.buffer->getBuffer() == std::string(1 << 16, 'a' + 60 % 26));
    }
    ZASSERT(db->write_compact_copy(keep) == index::BlobDatabase::Copy::Written);
    ZEXPECT(db->swap_compact_copy() == index::BlobDatabase::Swap::Replaced);
    ZEXPECT(db->usage()->pages < pages);
}

ZEST_CASE(OlderOpenerKeepsFile) {
    // An older clice reads the transaction numbering the copy restarts from
    // the `-lock` file it shares: the file stays while it has it open.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    auto db = sparse_database(store);
    auto pages = db->usage()->pages;
    OlderOpener opener(file);
    ZASSERT(db->write_compact_copy(keep) == index::BlobDatabase::Copy::Written);
    ZEXPECT(db->swap_compact_copy() == index::BlobDatabase::Swap::Kept);
    ZEXPECT(!fs::exists(file + "-compact"));
    ZEXPECT(db->usage()->pages == pages);
    auto value = db->read(index::BlobKind::Shard, "60");
    ZASSERT(bool(value));
    ZEXPECT(value.buffer->getBuffer() == std::string(1 << 16, 'a' + 60 % 26));
}

ZEST_CASE(ClosingRemovesCopy) {
    // A shrink cancelled between the copy and the swap leaves no copy.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    auto db = sparse_database(store);
    ZASSERT(db->write_compact_copy(keep) == index::BlobDatabase::Copy::Written);
    ZEXPECT(fs::exists(file + "-compact"));
    db.reset();
    ZEXPECT(!fs::exists(file + "-compact"));
}

ZEST_CASE(ClosingFitsFileToPages) {
    // A Windows writer's map extends the file to the map's size; the compact
    // copy and the close leave it at the pages the database declares, the
    // next open reading on from there.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    auto db = sparse_database(store);
    ZASSERT(db->write_compact_copy(keep) == index::BlobDatabase::Copy::Written);
    ZASSERT(db->swap_compact_copy() == index::BlobDatabase::Swap::Replaced);
    auto usage = db->usage();
    ZASSERT(usage);
    db.reset();
    auto status = fs::status(file);
    ZASSERT(status);
    ZEXPECT(status->size == usage->pages * usage->page_size);
    auto again = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(again != nullptr);
    ZEXPECT(again->contains(index::BlobKind::Shard, "63"));
}

ZEST_CASE(ReaderKeepsFileSizeOnClose) {
    // The file is cut back only while no other process has it open.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    auto db = sparse_database(store);
    auto before = fs::status(file);
    ZASSERT(before);
    {
        ForeignReader reader(file);
        db.reset();
        auto after = fs::status(file);
        ZASSERT(after);
        ZEXPECT(after->size == before->size);
    }
}

ZEST_CASE(NextWriteReusesFreedPages) {
    // The pages a write frees are reused by the next one, the file not
    // growing while values are rewritten in place of others.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    auto rewrite = [&](char fill) {
        std::vector<index::BlobDatabase::Blob> blobs;
        for(std::uint32_t i = 0; i < 32; i += 1) {
            blobs.push_back(blob(index::BlobKind::Shard, std::to_string(i), large_value(fill)));
        }
        ZASSERT(db->write(blobs, {}).has_value());
        ZASSERT(db->advance_read_snapshot().has_value());
        db->retire_old_snapshot();
        return db->usage()->pages;
    };
    rewrite('a');
    auto pages = rewrite('b');
    for(char fill = 'c'; fill < 'h'; fill += 1) {
        ZEXPECT(rewrite(fill) == pages);
    }
}

ZEST_CASE(UnchangedValueIsNotRewritten) {
    // A value put back unchanged keeps its pages: the write neither frees
    // nor takes one page per value (each takes three).
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    std::vector<index::BlobDatabase::Blob> blobs;
    for(std::uint32_t i = 0; i < 32; i += 1) {
        blobs.push_back(blob(index::BlobKind::Shard, std::to_string(i), large_value('a')));
    }
    ZASSERT(db->write(blobs, {}).has_value());
    auto before = db->usage();
    ZASSERT(before.has_value());
    ZASSERT(db->write(blobs, {}).has_value());
    auto after = db->usage();
    ZASSERT(after.has_value());
    ZEXPECT(after->free_pages < before->free_pages + 32);
    ZEXPECT(after->pages < before->pages + 32);
}

/// The value files of the database at `file` (search segments), by name.
std::vector<std::string> value_files(llvm::StringRef file) {
    std::vector<std::string> names;
    auto prefix = std::string(path::filename(file)) + "-value-";
    auto entries = fs::Directory::open(path::parent(file));
    ZASSERT(entries.has_value());
    for(const auto& entry: *entries) {
        if(entry && llvm::StringRef(entry->name).starts_with(prefix)) {
            names.push_back(entry->name);
        }
    }
    ranges::sort(names);
    return names;
}

ZEST_CASE(SegmentStoredInFile) {
    // A search segment is a value file of its own, the B-tree holding an
    // empty value under its key; removing the record removes the file.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    auto key = index::number_key(std::uint32_t(7));
    ZASSERT(db->write({blob(index::BlobKind::SearchSegment, key, large_value('s'))}, {}));
    ZASSERT(db->advance_read_snapshot().has_value());
    db->retire_old_snapshot();
    ZEXPECT(value_files(file).size() == 1u);
    ZEXPECT(db->usage()->overflow_pages == 0u);
    auto value = db->read(index::BlobKind::SearchSegment, key);
    ZASSERT(bool(value));
    ZEXPECT(value.buffer->getBuffer() == large_value('s'));
    ZEXPECT(db->count(index::BlobKind::SearchSegment) == 1u);

    // The next open reads it from its file too.
    db.reset();
    db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    value = db->read(index::BlobKind::SearchSegment, key);
    ZASSERT(bool(value));
    ZEXPECT(value.buffer->getBuffer() == large_value('s'));
    value = {};

    ZASSERT(db->write(
        {
    },
        {{index::BlobKind::SearchSegment, key}}));
    ZEXPECT(value_files(file).empty());
}

ZEST_CASE(ReaderKeepsSegmentFile) {
    // A reader's snapshot may name a segment a commit removes: its file
    // stays while another process has the database open.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    auto key = index::number_key(std::uint32_t(1));
    ZASSERT(db->write({blob(index::BlobKind::SearchSegment, key, large_value('a'))}, {}));
    {
        ForeignReader reader(file);
        ZASSERT(db->write(
            {
        },
            {{index::BlobKind::SearchSegment, key}}));
        ZEXPECT(value_files(file).size() == 1u);
    }
    ZASSERT(db->write({blob(index::BlobKind::Header, "header", "gg")}, {}));
    ZEXPECT(value_files(file).empty());
}

ZEST_CASE(OpenRemovesUnnamedSegmentFiles) {
    // A write that did not finish, or a session that ended before it could
    // remove a file, leaves files no record names: the next writer removes
    // them.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    auto named = index::number_key(std::uint32_t(2));
    {
        auto db = index::open_database(store, {}).value_or(nullptr);
        ZASSERT(db != nullptr);
        ZASSERT(db->write({blob(index::BlobKind::SearchSegment, named, large_value('b'))}, {}));
    }
    auto kept = value_files(file);
    ZASSERT(kept.size() == 1u);
    auto stray = path::join(path::parent(file), "index.mdb-value-4e0000000000000003");
    ZASSERT(!fs::write_file(stray, "x"));
    ZASSERT(!fs::write_file(path::join(path::parent(file), kept.front() + ".partial"), "x"));
    ZEXPECT(value_files(file).size() == 3u);
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    ZEXPECT(value_files(file) == kept);
    auto value = db->read(index::BlobKind::SearchSegment, named);
    ZASSERT(bool(value));
    ZEXPECT(value.buffer->getBuffer() == large_value('b'));
}

ZEST_CASE(OrphanWrittenAgainStays) {
    // A session killed between a segment's file and its commit leaves the
    // file under a number the next session writes again: removing the
    // orphan once no reader holds the database spares the file a commit
    // names again.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    auto key = index::number_key(std::uint32_t(4));
    std::unique_ptr<index::BlobDatabase> db;
    {
        ForeignReader reader(file);
        {
            auto first = index::open_database(store, {}).value_or(nullptr);
            ZASSERT(first != nullptr);
            ZASSERT(
                first->write({blob(index::BlobKind::SearchSegment, key, large_value('a'))}, {}));
            ZASSERT(first->write(
                {
            },
                {{index::BlobKind::SearchSegment, key}}));
        }
        ZASSERT(value_files(file).size() == 1u);
        db = index::open_database(store, {}).value_or(nullptr);
        ZASSERT(db != nullptr);
        ZASSERT(db->write({blob(index::BlobKind::SearchSegment, key, large_value('b'))}, {}));
    }
    ZASSERT(db->write({blob(index::BlobKind::Header, "header", "gg")}, {}));
    ZASSERT(db->advance_read_snapshot().has_value());
    db->retire_old_snapshot();
    auto value = db->read(index::BlobKind::SearchSegment, key);
    ZASSERT(bool(value));
    ZEXPECT(value.buffer->getBuffer() == large_value('b'));
}

ZEST_CASE(InlineSegmentOfOlderClice) {
    // A clice that kept the segments in the B-tree left their bytes inline
    // under their keys: they read as stored, for the search index's format
    // check to turn them down.
    TempDir tmp;
    std::string file;
    {
        auto store = open_store(tmp, "lmdb");
        file = populated_database(store, 1);
    }
    auto key = index::number_key(std::uint32_t(5));
    {
        MDB_env* env = nullptr;
        ZASSERT(mdb_env_create(&env) == 0);
        ZASSERT(mdb_env_open(env, file.c_str(), MDB_NOSUBDIR, 0644) == 0);
        MDB_txn* txn = nullptr;
        ZASSERT(mdb_txn_begin(env, nullptr, 0, &txn) == 0);
        MDB_dbi dbi;
        ZASSERT(mdb_dbi_open(txn, nullptr, 0, &dbi) == 0);
        // The record's key: its kind's prefix, then its own.
        auto encoded = "N" + key;
        auto bytes = large_value('o');
        MDB_val mk{encoded.size(), encoded.data()};
        MDB_val mv{bytes.size(), bytes.data()};
        ZASSERT(mdb_put(txn, dbi, &mk, &mv, 0) == 0);
        ZASSERT(mdb_txn_commit(txn) == 0);
        mdb_env_close(env);
    }
    auto store = open_store(tmp, "lmdb", /*read_only=*/true);
    auto db = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(db != nullptr);
    auto value = db->read(index::BlobKind::SearchSegment, key);
    ZASSERT(bool(value));
    ZEXPECT(value.buffer->getBuffer() == large_value('o'));
    std::uint32_t read = 0;
    db->for_each(index::BlobKind::SearchSegment, [&](llvm::StringRef, index::ReadBlob blob) {
        read += blob ? 1 : 0;
        return true;
    });
    ZEXPECT(read == 1u);
}

ZEST_CASE(SwapReopensSmall) {
    // The compact file reopens with a map its size calls for: on Windows,
    // where a writable map extends the file to the map, the old one would
    // grow it back.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    auto db = index::open_lmdb_database(store, {}, 512ull << 20).value_or(nullptr);
    ZASSERT(db != nullptr);
    ZASSERT(db->write({blob(index::BlobKind::Shard, "a", large_value('a'))}, {}));
    ZASSERT(db->write_compact_copy(keep) == index::BlobDatabase::Copy::Written);
    ZASSERT(db->swap_compact_copy() == index::BlobDatabase::Swap::Replaced);
    auto status = fs::status(file);
    ZASSERT(status);
    ZEXPECT(status->size <= (256ull << 20));
}

ZEST_CASE(ReaderHoldsOpenLock) {
    // A read-only open holds the `-open` file shared until it closes, which
    // keeps the writer from swapping the file under it.
    TempDir tmp;
    std::string file;
    {
        auto store = open_store(tmp, "lmdb");
        file = path::join(index::database_directory(store, {}), "index.mdb");
        auto db = index::open_database(store, {}).value_or(nullptr);
        ZASSERT(db != nullptr);
        ZASSERT(db->write({blob(index::BlobKind::Header, "header", "gg")}, {}));
    }
    auto exclusive_free = [&] {
        auto opened = fs::File::open(file + "-open", {.read = true, .write = true, .create = true});
        ZASSERT(opened.has_value());
        bool free = opened->try_lock().value_or(false);
        if(free) {
            opened->unlock();
        }
        return free;
    };
    auto store = open_store(tmp, "lmdb", /*read_only=*/true);
    auto reader = index::open_database(store, {}).value_or(nullptr);
    ZASSERT(reader != nullptr);
    ZEXPECT(!exclusive_free());
    reader.reset();
    ZEXPECT(exclusive_free());
}

ZEST_CASE(OpenRemovesStaleCopy) {
    // A copy a killed writer left behind goes at the next writable open.
    TempDir tmp;
    std::string file;
    {
        auto store = open_store(tmp, "lmdb");
        file = path::join(index::database_directory(store, {}), "index.mdb");
        auto db = index::open_database(store, {}).value_or(nullptr);
        ZASSERT(db != nullptr);
        ZASSERT(db->write({blob(index::BlobKind::Header, "header", "gg")}, {}));
    }
    ZASSERT(!fs::write_file(file + "-compact", "partial"));
    {
        auto store = open_store(tmp, "lmdb", /*read_only=*/true);
        ZASSERT(index::open_database(store, {}).has_value());
        ZEXPECT(fs::exists(file + "-compact"));
    }
    auto store = open_store(tmp, "lmdb");
    ZASSERT(index::open_database(store, {}).has_value());
    ZEXPECT(!fs::exists(file + "-compact"));
}

#ifndef _WIN32
ZEST_CASE(NoRoomForLock) {
    // A disk without room for LMDB's lock file turns its first write into
    // a SIGBUS; a file size limit stands in for the full disk. It leaves
    // room for the lock file of 64 KiB, not for the probe of 256: only the
    // probe refuses the open.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto file = path::join(index::database_directory(store, {}), "index.mdb");
    std::unique_ptr<index::BlobDatabase> db;
    {
        struct rlimit saved = {};
        ZASSERT(::getrlimit(RLIMIT_FSIZE, &saved) == 0);
        struct sigaction ignored = {};
        ignored.sa_handler = SIG_IGN;
        struct sigaction previous = {};
        ZASSERT(::sigaction(SIGXFSZ, &ignored, &previous) == 0);
        auto restore = llvm::make_scope_exit([&] {
            ::setrlimit(RLIMIT_FSIZE, &saved);
            ::sigaction(SIGXFSZ, &previous, nullptr);
        });
        struct rlimit lowered = saved;
        lowered.rlim_cur = 128 * 1024;
        ZASSERT(::setrlimit(RLIMIT_FSIZE, &lowered) == 0);
        db = index::open_database(store, {}).value_or(nullptr);
    }
    ZEXPECT(db == nullptr);
    ZEXPECT(!fs::exists(file + "-lock"));
    ZEXPECT(!fs::exists(file + "-probe"));
    ZEXPECT(index::open_database(store, {}).value_or(nullptr) != nullptr);
}
#endif

};  // ZEST_SUITE(IndexDatabase)

}  // namespace
}  // namespace clice::testing
