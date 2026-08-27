#include <cstdlib>
#include <print>

#include "test/temp_dir.h"
#include "test/test.h"
#include "index/database.h"
#include "support/cache_store.h"
#include "support/filesystem.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::testing {
namespace {

constexpr std::uint32_t version = 1;
constexpr llvm::StringLiteral backends[] = {"files", "lmdb"};

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

TEST_SUITE(IndexDatabase) {

TEST_CASE(WriteReadRoundTrip) {
    TempDir tmp;
    for(auto backend: backends) {
        auto store = open_store(tmp, backend);
        auto db = index::open_database(store, backend);
        ASSERT_TRUE(db != nullptr);

        auto rejected = db->write({blob(index::IndexBlobKind::Shard, "a", large_value('a')),
                                   blob(index::IndexBlobKind::Global, "global", "gg")},
                                  {});
        ASSERT_TRUE(rejected.empty());
        // Reads serve the resident snapshot; committed writes become
        // visible only after an advance (a no-op on the files backend).
        ASSERT_TRUE(db->advance_read_snapshot().has_value());
        db->retire_old_snapshot();

        auto shard = db->read(index::IndexBlobKind::Shard, "a");
        ASSERT_TRUE(bool(shard));
        ASSERT_TRUE(shard.buffer->getBuffer() == large_value('a'));
        ASSERT_TRUE(db->contains(index::IndexBlobKind::Global, "global"));
        ASSERT_FALSE(db->contains(index::IndexBlobKind::Shard, "missing"));
        ASSERT_FALSE(bool(db->read(index::IndexBlobKind::Shard, "missing")));
    }
}

TEST_CASE(WriteRemoves) {
    TempDir tmp;
    for(auto backend: backends) {
        auto store = open_store(tmp, backend);
        auto db = index::open_database(store, backend);
        ASSERT_TRUE(db != nullptr);

        ASSERT_TRUE(db->write({blob(index::IndexBlobKind::Manifest, "m1", "one"),
                               blob(index::IndexBlobKind::Manifest, "m2", "two")},
                              {})
                        .empty());
        ASSERT_TRUE(db->write(
                          {
        },
                          {{index::IndexBlobKind::Manifest, "m1"}})
                        .empty());
        ASSERT_TRUE(db->advance_read_snapshot().has_value());
        db->retire_old_snapshot();

        ASSERT_FALSE(db->contains(index::IndexBlobKind::Manifest, "m1"));
        ASSERT_TRUE(db->contains(index::IndexBlobKind::Manifest, "m2"));
    }
}

TEST_CASE(KindsAreIsolated) {
    TempDir tmp;
    for(auto backend: backends) {
        auto store = open_store(tmp, backend);
        auto db = index::open_database(store, backend);
        ASSERT_TRUE(db != nullptr);

        ASSERT_TRUE(db->write({blob(index::IndexBlobKind::Shard, "same", "shard"),
                               blob(index::IndexBlobKind::Manifest, "same", "manifest")},
                              {})
                        .empty());
        ASSERT_TRUE(db->advance_read_snapshot().has_value());
        db->retire_old_snapshot();

        llvm::SmallVector<std::string> shard_keys;
        db->for_each_key(index::IndexBlobKind::Shard,
                         [&](llvm::StringRef key) { shard_keys.push_back(key.str()); });
        ASSERT_TRUE(shard_keys.size() == 1);
        ASSERT_TRUE(shard_keys.front() == "same");
        ASSERT_TRUE(db->read(index::IndexBlobKind::Manifest, "same").buffer->getBuffer() ==
                    "manifest");
        ASSERT_FALSE(db->contains(index::IndexBlobKind::Global, "same"));
    }
}

TEST_CASE(SnapshotPinsUntilAdvance) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, "lmdb");
    ASSERT_TRUE(db != nullptr);

    ASSERT_TRUE(db->write({blob(index::IndexBlobKind::Shard, "k", large_value('1'))}, {}).empty());
    ASSERT_TRUE(db->advance_read_snapshot().has_value());
    db->retire_old_snapshot();

    auto before = db->read(index::IndexBlobKind::Shard, "k");
    ASSERT_TRUE(bool(before));

    // Committed writes stay invisible to the resident snapshot until the
    // next advance; the borrowed buffer keeps serving the old bytes.
    ASSERT_TRUE(db->write({blob(index::IndexBlobKind::Shard, "k", large_value('2'))}, {}).empty());
    ASSERT_TRUE(db->read(index::IndexBlobKind::Shard, "k").buffer->getBuffer() == large_value('1'));
    ASSERT_TRUE(before.buffer->getBuffer() == large_value('1'));

    auto advanced = db->advance_read_snapshot();
    ASSERT_TRUE(advanced.has_value());
    ASSERT_TRUE(*advanced != 0);
    // Old and new snapshots serve their own bytes side by side until the
    // old one retires — the migration window's core invariant.
    ASSERT_TRUE(db->read(index::IndexBlobKind::Shard, "k").buffer->getBuffer() == large_value('2'));
    ASSERT_TRUE(before.buffer->getBuffer() == large_value('1'));
    db->retire_old_snapshot();
}

TEST_CASE(SmallValuesCopiedAligned) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, "lmdb");
    ASSERT_TRUE(db != nullptr);

    ASSERT_TRUE(db->write({blob(index::IndexBlobKind::Manifest, "small", "tiny"),
                           blob(index::IndexBlobKind::Shard, "big", large_value('b'))},
                          {})
                    .empty());
    ASSERT_TRUE(db->advance_read_snapshot().has_value());
    db->retire_old_snapshot();

    // Inline leaf values are only 2-aligned, so they must come back as an
    // owned copy (generation 0); overflow-page values are borrowed.
    auto small = db->read(index::IndexBlobKind::Manifest, "small");
    ASSERT_TRUE(bool(small));
    ASSERT_TRUE(small.generation == 0);
    ASSERT_TRUE(reinterpret_cast<std::uintptr_t>(small.buffer->getBufferStart()) % 8 == 0);

    auto big = db->read(index::IndexBlobKind::Shard, "big");
    ASSERT_TRUE(bool(big));
    ASSERT_TRUE(big.generation != 0);
    ASSERT_TRUE(reinterpret_cast<std::uintptr_t>(big.buffer->getBufferStart()) % 8 == 0);
}

TEST_CASE(ReopenServesPersistedBlobs) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    {
        auto db = index::open_database(store, "lmdb");
        ASSERT_TRUE(db != nullptr);
        ASSERT_TRUE(db->write({blob(index::IndexBlobKind::CDB, "cdb", "snapshot")}, {}).empty());
    }
    auto db = index::open_database(store, "lmdb");
    ASSERT_TRUE(db != nullptr);
    ASSERT_TRUE(db->read(index::IndexBlobKind::CDB, "cdb").buffer->getBuffer() == "snapshot");
}

TEST_CASE(CorruptDatabaseRebuilds) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    {
        std::error_code ec;
        llvm::raw_fd_ostream os(path::join(store.base_dir(), "index.mdb"), ec);
        require(!ec, "writing garbage failed");
        os << "this is not an lmdb file, not even close, but long enough to map";
    }
    auto db = index::open_database(store, "lmdb");
    ASSERT_TRUE(db != nullptr);
    ASSERT_FALSE(db->contains(index::IndexBlobKind::CDB, "cdb"));
    ASSERT_TRUE(db->write({blob(index::IndexBlobKind::CDB, "cdb", "fresh")}, {}).empty());
}

TEST_CASE(DefaultOpenFileBounded) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, "lmdb");
    ASSERT_TRUE(db != nullptr);
    ASSERT_TRUE(db->write({blob(index::IndexBlobKind::CDB, "cdb", "x")}, {}).empty());

    // On Windows the mapping extends index.mdb to the whole mapsize, so
    // this pins the small default (a 64 GiB logical file is the bug the
    // default exists to avoid); POSIX file sizes track the data
    // high-water mark and pass trivially.
    std::uint64_t size = 0;
    ASSERT_TRUE(!llvm::sys::fs::file_size(path::join(store.base_dir(), "index.mdb"), size));
    ASSERT_TRUE(size <= 256ull << 20);
}

TEST_CASE(FullMapFailsWholeBatchThenGrows) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    // Small enough that a handful of large values exhausts it.
    auto db = index::open_lmdb_database(store, 256 * 1024);
    ASSERT_TRUE(db != nullptr);

    std::vector<index::BlobDatabase::Blob> puts;
    for(int i = 0; i < 64; i += 1) {
        puts.push_back(blob(index::IndexBlobKind::Shard, std::to_string(i), large_value('x')));
    }
    auto rejected = db->write(puts, {});
    ASSERT_TRUE(rejected.size() == puts.size());
    ASSERT_FALSE(db->contains(index::IndexBlobKind::Shard, "0"));

    auto grown = db->grow();
    ASSERT_TRUE(grown.has_value());
    ASSERT_TRUE(*grown);
    // grow() opened a fresh snapshot, so this proves the failed batch
    // really committed nothing (the pre-grow check only saw the pinned
    // old snapshot).
    ASSERT_FALSE(db->contains(index::IndexBlobKind::Shard, "0"));
    // A second grow without a latched full map is a no-op.
    auto again = db->grow();
    ASSERT_TRUE(again.has_value());
    ASSERT_FALSE(*again);

    ASSERT_TRUE(db->write(puts, {}).empty());
    ASSERT_TRUE(db->advance_read_snapshot().has_value());
    db->retire_old_snapshot();
    ASSERT_TRUE(db->read(index::IndexBlobKind::Shard, "63").buffer->getBuffer() ==
                large_value('x'));
}

TEST_CASE(ReadOnlyWithoutDatabaseFallsBack) {
    TempDir tmp;
    { auto store = open_store(tmp, "empty"); }
    auto store = open_store(tmp, "empty", /*read_only=*/true);
    // A reader before any LMDB writer ran gets the filesystem view (empty
    // here) instead of failing on a missing index.mdb.
    auto db = index::open_database(store, "lmdb");
    ASSERT_TRUE(db != nullptr);
    ASSERT_FALSE(db->contains(index::IndexBlobKind::Global, "global"));
}

TEST_CASE(ReadOnlyServesExistingDatabase) {
    TempDir tmp;
    {
        auto store = open_store(tmp, "ws");
        auto db = index::open_database(store, "lmdb");
        ASSERT_TRUE(db != nullptr);
        ASSERT_TRUE(db->write({blob(index::IndexBlobKind::Global, "global", "gg")}, {}).empty());
    }
    auto store = open_store(tmp, "ws", /*read_only=*/true);
    auto db = index::open_database(store, "lmdb");
    ASSERT_TRUE(db != nullptr);
    ASSERT_TRUE(db->contains(index::IndexBlobKind::Global, "global"));
    ASSERT_TRUE(db->read(index::IndexBlobKind::Global, "global").buffer->getBuffer() == "gg");
}

TEST_CASE(CondemnedDatabaseDeletesOnClose) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    {
        auto db = index::open_database(store, "lmdb");
        ASSERT_TRUE(db != nullptr);
        ASSERT_TRUE(db->write({blob(index::IndexBlobKind::CDB, "cdb", "bytes")}, {}).empty());
        db->condemn();
    }
    ASSERT_FALSE(llvm::sys::fs::exists(path::join(store.base_dir(), "index.mdb")));
    auto db = index::open_database(store, "lmdb");
    ASSERT_TRUE(db != nullptr);
    ASSERT_FALSE(db->contains(index::IndexBlobKind::CDB, "cdb"));
}

TEST_CASE(OutstandingSnapshotsStack) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, "lmdb");
    ASSERT_TRUE(db != nullptr);

    ASSERT_TRUE(db->write({blob(index::IndexBlobKind::Shard, "k", large_value('1'))}, {}).empty());
    ASSERT_TRUE(db->advance_read_snapshot().has_value());
    db->retire_old_snapshot();
    auto lease = db->read(index::IndexBlobKind::Shard, "k");
    ASSERT_TRUE(bool(lease));

    // A cancelled migration leaves its old snapshot outstanding and the
    // next advance stacks another; every stacked snapshot keeps its
    // borrowers alive until one retire clears them all.
    ASSERT_TRUE(db->write({blob(index::IndexBlobKind::Shard, "k", large_value('2'))}, {}).empty());
    ASSERT_TRUE(db->advance_read_snapshot().has_value());
    ASSERT_TRUE(db->write({blob(index::IndexBlobKind::Shard, "k", large_value('3'))}, {}).empty());
    ASSERT_TRUE(db->advance_read_snapshot().has_value());
    ASSERT_TRUE(lease.buffer->getBuffer() == large_value('1'));
    ASSERT_TRUE(db->read(index::IndexBlobKind::Shard, "k").buffer->getBuffer() == large_value('3'));
    db->retire_old_snapshot();
    ASSERT_TRUE(db->read(index::IndexBlobKind::Shard, "k").buffer->getBuffer() == large_value('3'));
}

TEST_CASE(UnknownBackendFallsBackToLmdb) {
    TempDir tmp;
    auto store = open_store(tmp, "lmdb");
    auto db = index::open_database(store, "bogus");
    ASSERT_TRUE(db != nullptr);
    ASSERT_TRUE(llvm::sys::fs::exists(path::join(store.base_dir(), "index.mdb")));
}

};  // TEST_SUITE(IndexDatabase)

}  // namespace
}  // namespace clice::testing
