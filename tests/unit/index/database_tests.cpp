#include <cstdlib>
#include <print>

#include "lmdb.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "index/database.h"
#include "index/writer_lock.h"
#include "support/cache_store.h"
#include "vfs/path.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::testing {
namespace {

constexpr std::uint32_t version = 1;

/// Helper precondition check that survives NDEBUG builds; failures abort
/// with a message instead of becoming UB on a bad expected access.
void require(bool condition, const char* what) {
    if(!condition) {
        std::println(stderr, "database_tests: requirement failed: {}", what);
        std::abort();
    }
}

CacheStore open_store(TempDir& tmp, llvm::StringRef sub, bool read_only = false) {
    auto store = CacheStore::open(tmp.path(sub), version, read_only);
    require(store.has_value(), "CacheStore::open failed");
    return std::move(*store);
}

index::BlobDatabase::Blob blob(index::IndexBlobKind kind, llvm::StringRef key, std::string bytes) {
    return {kind, key.str(), std::move(bytes)};
}

/// Value sizes on the two LMDB layouts: large lands on overflow pages
/// (16-aligned, served borrowed), small stays inline in the leaf
/// (2-aligned at worst, served as an owned copy).
std::string large_value(char fill) {
    return std::string(8192, fill);
}

ZEST_SUITE(IndexDatabase) {

ZEST_CASE(WriteReadRoundTrip) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);

    auto rejected = db->write({blob(index::IndexBlobKind::Shard, "a", large_value('a')),
                               blob(index::IndexBlobKind::Global, "global", "gg")},
                              {});
    ZASSERT(rejected.empty());
    // Reads serve the resident snapshot; committed writes become
    // visible only after an advance.
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();

    auto shard = db->read(index::IndexBlobKind::Shard, "a");
    ZASSERT(bool(shard));
    ZASSERT(shard.buffer->getBuffer() == large_value('a'));
    ZASSERT(db->contains(index::IndexBlobKind::Global, "global"));
    ZASSERT(!db->contains(index::IndexBlobKind::Shard, "missing"));
    ZASSERT(!bool(db->read(index::IndexBlobKind::Shard, "missing")));
}

ZEST_CASE(WriteRemoves) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);

    ZASSERT(db->write({blob(index::IndexBlobKind::Manifest, "m1", "one"),
                       blob(index::IndexBlobKind::Manifest, "m2", "two")},
                      {})
                .empty());
    ZASSERT(db->write(
                  {
    },
                  {{index::IndexBlobKind::Manifest, "m1"}})
                .empty());
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();

    ZASSERT(!db->contains(index::IndexBlobKind::Manifest, "m1"));
    ZASSERT(db->contains(index::IndexBlobKind::Manifest, "m2"));
}

ZEST_CASE(KindsAreIsolated) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);

    ZASSERT(db->write({blob(index::IndexBlobKind::Shard, "same", "shard"),
                       blob(index::IndexBlobKind::Manifest, "same", "manifest")},
                      {})
                .empty());
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();

    llvm::SmallVector<std::string> shard_keys;
    db->for_each_key(index::IndexBlobKind::Shard,
                     [&](llvm::StringRef key) { shard_keys.push_back(key.str()); });
    ZASSERT(shard_keys.size() == 1);
    ZASSERT(shard_keys.front() == "same");
    ZASSERT(db->read(index::IndexBlobKind::Manifest, "same").buffer->getBuffer() == "manifest");
    ZASSERT(!db->contains(index::IndexBlobKind::Global, "same"));
}

ZEST_CASE(SnapshotPinsUntilAdvance) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);

    ZASSERT(db->write({blob(index::IndexBlobKind::Shard, "k", large_value('1'))}, {}).empty());
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();

    auto before = db->read(index::IndexBlobKind::Shard, "k");
    ZASSERT(bool(before));

    // Committed writes stay invisible to the resident snapshot until the
    // next advance; the borrowed buffer keeps serving the old bytes.
    ZASSERT(db->write({blob(index::IndexBlobKind::Shard, "k", large_value('2'))}, {}).empty());
    ZASSERT(db->read(index::IndexBlobKind::Shard, "k").buffer->getBuffer() == large_value('1'));
    ZASSERT(before.buffer->getBuffer() == large_value('1'));

    auto advanced = db->advance_read_snapshot();
    ZASSERT(advanced);
    ZASSERT(*advanced != 0);
    // Old and new snapshots serve their own bytes side by side until the
    // old one retires — the migration window's core invariant.
    ZASSERT(db->read(index::IndexBlobKind::Shard, "k").buffer->getBuffer() == large_value('2'));
    ZASSERT(before.buffer->getBuffer() == large_value('1'));
    db->retire_old_snapshot();
}

ZEST_CASE(SmallValuesCopiedAligned) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);

    ZASSERT(db->write({blob(index::IndexBlobKind::Manifest, "small", "tiny"),
                       blob(index::IndexBlobKind::Shard, "big", large_value('b'))},
                      {})
                .empty());
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();

    // Inline leaf values are only 2-aligned, so they must come back as an
    // owned copy (generation 0); overflow-page values are borrowed.
    auto small = db->read(index::IndexBlobKind::Manifest, "small");
    ZASSERT(bool(small));
    ZASSERT(small.generation == 0);
    ZASSERT(reinterpret_cast<std::uintptr_t>(small.buffer->getBufferStart()) % 8 == 0);

    auto big = db->read(index::IndexBlobKind::Shard, "big");
    ZASSERT(bool(big));
    ZASSERT(big.generation != 0);
    ZASSERT(reinterpret_cast<std::uintptr_t>(big.buffer->getBufferStart()) % 8 == 0);
}

ZEST_CASE(ReopenServesPersistedBlobs) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    {
        auto db = index::open_database(store, "");
        ZASSERT(db != nullptr);
        ZASSERT(db->write({blob(index::IndexBlobKind::CDB, "cdb", "snapshot")}, {}).empty());
    }
    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);
    ZASSERT(db->read(index::IndexBlobKind::CDB, "cdb").buffer->getBuffer() == "snapshot");
}

ZEST_CASE(CorruptDatabaseRebuilds) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    {
        auto library = index::library_directory(store, "");
        require(!llvm::sys::fs::create_directories(library), "creating the library failed");
        std::error_code ec;
        llvm::raw_fd_ostream os(path::join(library, "index.mdb"), ec);
        require(!ec, "writing garbage failed");
        os << "this is not an lmdb file, not even close, but long enough to map";
    }
    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);
    ZASSERT(!db->contains(index::IndexBlobKind::CDB, "cdb"));
    ZASSERT(db->write({blob(index::IndexBlobKind::CDB, "cdb", "fresh")}, {}).empty());
}

/// A database of `count` large blobs written over several commits, so
/// its pages run well past the two meta pages; returns the file's path.
std::string populated_database(CacheStore& store, int count) {
    auto db = index::open_database(store, "");
    require(db != nullptr, "opening the database failed");
    for(int i = 0; i < count; i += 1) {
        auto rejected =
            db->write({blob(index::IndexBlobKind::Shard, std::to_string(i), large_value('t'))}, {});
        require(rejected.empty(), "writing a blob failed");
    }
    return path::join(index::library_directory(store, ""), "index.mdb");
}

/// Cuts the file down to its two meta pages: the tree they point at is gone.
void truncate_to_meta(llvm::StringRef file) {
    int fd = -1;
    require(!llvm::sys::fs::openFileForReadWrite(file,
                                                 fd,
                                                 llvm::sys::fs::CD_OpenExisting,
                                                 llvm::sys::fs::OF_None),
            "opening index.mdb failed");
    require(!llvm::sys::fs::resize_file(fd, 2 * llvm::sys::Process::getPageSizeEstimate()),
            "truncating index.mdb failed");
    llvm::sys::Process::SafelyCloseFileDescriptor(fd);
}

ZEST_CASE(TruncatedDatabaseRebuilds) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    truncate_to_meta(populated_database(store, 8));

    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);
    ZASSERT(!db->contains(index::IndexBlobKind::Shard, "7"));
    ZASSERT(db->write({blob(index::IndexBlobKind::CDB, "cdb", "fresh")}, {}).empty());
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();
    ZASSERT(db->read(index::IndexBlobKind::CDB, "cdb").buffer->getBuffer() == "fresh");
}

ZEST_CASE(ReadOnlyRefusesTruncated) {
    TempDir tmp;
    {
        auto store = open_store(tmp, "ws");
        truncate_to_meta(populated_database(store, 8));
    }
    auto store = open_store(tmp, "ws", /*read_only=*/true);
    ZASSERT(index::open_database(store, "") == nullptr);
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
        file = path::join(index::library_directory(store, ""), "index.mdb");
        auto db = index::open_database(store, "");
        ZASSERT(db != nullptr);
        auto settle = [&] {
            ZASSERT(db->advance_read_snapshot());
            db->retire_old_snapshot();
        };
        ZASSERT(db->write({blob(index::IndexBlobKind::Shard, "x", large_value('x'))}, {}).empty());
        settle();
        ZASSERT(db->write(
                      {
        },
                      {{index::IndexBlobKind::Shard, "x"}})
                    .empty());
        settle();
        ZASSERT(db->write(
                      {
                          blob(index::IndexBlobKind::CDB, "cdb", "small"),
                          blob(index::IndexBlobKind::Shard, "h", std::string(1 << 16, 'h'))
        },
                      {{index::IndexBlobKind::Shard, "h"}})
                    .empty());
    }

    MDB_env* env = nullptr;
    ZASSERT(mdb_env_create(&env) == 0);
    ZASSERT(mdb_env_open(env, file.c_str(), MDB_NOSUBDIR | MDB_RDONLY, 0644) == 0);
    MDB_envinfo info;
    mdb_env_info(env, &info);
    MDB_stat db_stat;
    mdb_env_stat(env, &db_stat);
    mdb_env_close(env);
    std::uint64_t size = 0;
    ZASSERT(!static_cast<bool>(llvm::sys::fs::file_size(file, size)));
    ZEXPECT(size >= (info.me_last_pgno + 1) * db_stat.ms_psize);

    auto store = open_store(tmp, "lmdb", /*read_only=*/true);
    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);
    ZASSERT(db->contains(index::IndexBlobKind::CDB, "cdb"));
}

ZEST_CASE(DefaultOpenFileBounded) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);
    ZASSERT(db->write({blob(index::IndexBlobKind::CDB, "cdb", "x")}, {}).empty());

    // On Windows the mapping extends index.mdb to the whole mapsize, so
    // this pins the small default (a 64 GiB logical file is the bug the
    // default exists to avoid); POSIX file sizes track the data
    // high-water mark and pass trivially.
    std::uint64_t size = 0;
    ZASSERT(!llvm::sys::fs::file_size(path::join(index::library_directory(store, ""), "index.mdb"),
                                      size));
    ZASSERT((size <= 256ull << 20));
}

ZEST_CASE(FullMapFailsWholeBatchThenGrows) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    // Small enough that a handful of large values exhausts it.
    auto db = index::open_lmdb_database(store, "", 256 * 1024);
    ZASSERT(db != nullptr);

    std::vector<index::BlobDatabase::Blob> puts;
    for(int i = 0; i < 64; i += 1) {
        puts.push_back(blob(index::IndexBlobKind::Shard, std::to_string(i), large_value('x')));
    }
    auto rejected = db->write(puts, {});
    ZASSERT(rejected.size() == puts.size());
    ZASSERT(!db->contains(index::IndexBlobKind::Shard, "0"));

    auto grown = db->grow();
    ZASSERT(grown);
    ZASSERT(*grown);
    // grow() opened a fresh snapshot, so this proves the failed batch
    // really committed nothing (the pre-grow check only saw the pinned
    // old snapshot).
    ZASSERT(!db->contains(index::IndexBlobKind::Shard, "0"));
    // A second grow without a latched full map is a no-op.
    auto again = db->grow();
    ZASSERT(again);
    ZASSERT(!*again);

    ZASSERT(db->write(puts, {}).empty());
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();
    ZASSERT(db->read(index::IndexBlobKind::Shard, "63").buffer->getBuffer() == large_value('x'));
}

ZEST_CASE(ReadOnlyMissingDatabase) {
    TempDir tmp;
    { auto store = open_store(tmp, "empty"); }
    auto store = open_store(tmp, "empty", /*read_only=*/true);
    // A reader before any writer ran has nothing to read: persistence is
    // disabled rather than creating an index.mdb a read-only session must
    // not leave behind.
    auto db = index::open_database(store, "", /*read_only=*/true);
    ZASSERT(db == nullptr);
    ZASSERT(!llvm::sys::fs::exists(index::library_directory(store, "")));
}

ZEST_CASE(ReadOnlyServesExistingDatabase) {
    TempDir tmp;
    {
        auto store = open_store(tmp, "ws");
        auto db = index::open_database(store, "");
        ZASSERT(db != nullptr);
        ZASSERT(db->write({blob(index::IndexBlobKind::Global, "global", "gg")}, {}).empty());
    }
    auto store = open_store(tmp, "ws", /*read_only=*/true);
    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);
    ZASSERT(db->contains(index::IndexBlobKind::Global, "global"));
    ZASSERT(db->read(index::IndexBlobKind::Global, "global").buffer->getBuffer() == "gg");
}

ZEST_CASE(CondemnedDatabaseDeletesOnClose) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    {
        auto db = index::open_database(store, "");
        ZASSERT(db != nullptr);
        ZASSERT(db->write({blob(index::IndexBlobKind::CDB, "cdb", "bytes")}, {}).empty());
        db->condemn();
    }
    ZASSERT(!llvm::sys::fs::exists(path::join(index::library_directory(store, ""), "index.mdb")));
    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);
    ZASSERT(!db->contains(index::IndexBlobKind::CDB, "cdb"));
}

ZEST_CASE(LibraryPerConfiguration) {
    /// Every configuration has a library of its own under `index/`, the
    /// anonymous one named `default`; what one holds the other never sees.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    ZEXPECT(index::library_directory(store, "") ==
            path::join(store.base_dir(), "index", "default"));
    auto library = index::library_directory(store, "release");
    ZEXPECT(path::parent_path(library) == path::join(store.base_dir(), "index"));
    ZEXPECT(path::filename(library).starts_with("release~"));

    auto debug = index::open_database(store, "debug");
    auto release = index::open_database(store, "release");
    ZASSERT(debug != nullptr);
    ZASSERT(release != nullptr);
    ZASSERT(debug->write({blob(index::IndexBlobKind::Global, "global", "d")}, {}).empty());
    ZASSERT(release->advance_read_snapshot());
    ZASSERT(!release->contains(index::IndexBlobKind::Global, "global"));
    ZASSERT(
        llvm::sys::fs::exists(path::join(index::library_directory(store, "debug"), "index.mdb")));
    ZASSERT(
        llvm::sys::fs::exists(path::join(index::library_directory(store, "release"), "index.mdb")));
}

ZEST_CASE(LibraryNameSanitized) {
    /// The name's prefix is the tag reduced to a safe lowercase spelling;
    /// the hash behind the `~` tells apart tags that reduce alike, alias
    /// each other on a case-folding filesystem, or spell `default`.
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto name = [&](llvm::StringRef configuration) {
        return path::filename(index::library_directory(store, configuration)).str();
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

ZEST_CASE(LibraryBlockedByFile) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto library = index::library_directory(store, "x");
    auto ec = llvm::sys::fs::create_directories(path::parent_path(library));
    ZASSERT(!ec);
    ZASSERT(!vfs::write(library, "x"));
    ZASSERT(index::open_database(store, "x") == nullptr);
}

ZEST_CASE(WriterLockAtCacheRoot) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto lock = index::WriterLock::acquire(store.root_dir());
    ZASSERT(lock);
    // The lock guards the cache directory, not a configuration's library,
    // and opening a library takes none. (Contention itself is between
    // processes: POSIX record locks are per process, so a second
    // acquisition in this one would succeed.)
    ZASSERT(llvm::sys::fs::exists(path::join(store.root_dir(), "index.lock")));
    auto db = index::open_database(store, "x");
    ZASSERT(db != nullptr);
    ZASSERT(!llvm::sys::fs::exists(path::join(index::library_directory(store, "x"), "index.lock")));
#ifndef _WIN32
    auto held = read_file(path::join(store.root_dir(), "index.lock"));
    ZASSERT(held);
    ZASSERT(llvm::StringRef(*held).trim() == std::to_string(llvm::sys::Process::getProcessId()));
#endif
    lock.reset();
    auto released = read_file(path::join(store.root_dir(), "index.lock"));
    ZASSERT((released.has_value() && released->empty()));
}

ZEST_CASE(ProbeIgnoresStaleEndpoint) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto cache_dir = store.root_dir();
    index::write_endpoint(cache_dir, {.pid = 1, .version = "x", .host = "127.0.0.1", .port = 1});
    ZASSERT(llvm::sys::fs::exists(path::join(cache_dir, "server.json")));

    // Nobody holds the lock: the record is a crash's residue, swept by
    // the probe.
    auto probe = index::probe_writer(cache_dir);
    ZASSERT(probe.state == index::WriterProbe::State::Free);
    ZASSERT(!llvm::sys::fs::exists(path::join(cache_dir, "server.json")));

    index::write_endpoint(cache_dir, {.pid = 1, .version = "x", .host = "127.0.0.1", .port = 1});
    index::remove_endpoint(cache_dir);
    ZASSERT(!llvm::sys::fs::exists(path::join(cache_dir, "server.json")));
}

ZEST_CASE(OutstandingSnapshotsStack) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, "");
    ZASSERT(db != nullptr);

    ZASSERT(db->write({blob(index::IndexBlobKind::Shard, "k", large_value('1'))}, {}).empty());
    ZASSERT(db->advance_read_snapshot());
    db->retire_old_snapshot();
    auto lease = db->read(index::IndexBlobKind::Shard, "k");
    ZASSERT(bool(lease));

    // A cancelled migration leaves its old snapshot outstanding and the
    // next advance stacks another; every stacked snapshot keeps its
    // borrowers alive until one retire clears them all.
    ZASSERT(db->write({blob(index::IndexBlobKind::Shard, "k", large_value('2'))}, {}).empty());
    ZASSERT(db->advance_read_snapshot());
    ZASSERT(db->write({blob(index::IndexBlobKind::Shard, "k", large_value('3'))}, {}).empty());
    ZASSERT(db->advance_read_snapshot());
    ZASSERT(lease.buffer->getBuffer() == large_value('1'));
    ZASSERT(db->read(index::IndexBlobKind::Shard, "k").buffer->getBuffer() == large_value('3'));
    db->retire_old_snapshot();
    ZASSERT(db->read(index::IndexBlobKind::Shard, "k").buffer->getBuffer() == large_value('3'));
}

};  // ZEST_SUITE(IndexDatabase)

}  // namespace
}  // namespace clice::testing
