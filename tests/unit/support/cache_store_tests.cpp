#include <cstdlib>
#include <format>
#include <print>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "test/temp_dir.h"
#include "test/test.h"
#include "support/cache_store.h"
#include "support/filesystem.h"

#include "llvm/Support/Process.h"

namespace clice::testing {

namespace {

constexpr std::uint32_t version = 1;

/// A pid no real process can have (Linux pid_max is 4194304; Windows
/// pids are multiples of 4), so its directories always look dead.
constexpr const char* dead_pid = "999999999";

/// Helper precondition check that survives NDEBUG builds (the unit tests
/// run under RelWithDebInfo); failures abort with a location instead of
/// becoming UB on a bad expected/optional access.
void require(bool condition, const char* what) {
    if(!condition) {
        std::println(stderr, "cache_store_tests: requirement failed: {}", what);
        std::abort();
    }
}

CacheStore open_store(const TempDir& tmp, std::uint32_t ver = version) {
    auto store = CacheStore::open(tmp.path("root"), ver);
    require(store.has_value(), "CacheStore::open failed");
    return std::move(*store);
}

void register_lru(CacheStore& store, std::uint64_t max_bytes = 0) {
    store.register_namespace(
        {.name = "pch", .extension = ".pch", .policy = CachePolicy::LRU, .max_bytes = max_bytes});
}

void register_paired(CacheStore& store, std::uint64_t max_bytes = 0) {
    store.register_namespace({.name = "pch",
                              .extension = ".pch",
                              .aux_extension = ".pch.idx",
                              .policy = CachePolicy::LRU,
                              .max_bytes = max_bytes});
}

/// Run a full two-phase aux write with the given content.
std::string
    put_aux(CacheStore& store, llvm::StringRef ns, llvm::StringRef key, llvm::StringRef content) {
    auto pending = store.begin_store_aux(ns, key);
    require(!pending.tmp_path.empty(), "begin_store_aux returned no tmp path");
    require(fs::write(pending.tmp_path, content).has_value(), "tmp write failed");
    auto committed = store.commit(std::move(pending));
    require(committed.has_value(), "aux commit failed");
    return *committed;
}

/// Run a full two-phase write with the given content.
std::string
    put(CacheStore& store, llvm::StringRef ns, llvm::StringRef key, llvm::StringRef content) {
    auto pending = store.begin_store(ns, key);
    require(!pending.tmp_path.empty(), "begin_store returned no tmp path");
    require(fs::write(pending.tmp_path, content).has_value(), "tmp write failed");
    auto committed = store.commit(std::move(pending));
    require(committed.has_value(), "commit failed");
    return *committed;
}

TEST_SUITE(CacheStore) {

TEST_CASE(StoreAndLookup) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    ASSERT_FALSE(store.lookup("pch", "k1").has_value());

    auto path = put(store, "pch", "k1", "blob content");

    auto hit = store.lookup("pch", "k1");
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(*hit, path);
    ASSERT_EQ(fs::read(*hit).value_or(""), "blob content");

    // The blob landed inside the versioned namespace directory.
    ASSERT_TRUE(llvm::StringRef(path).contains("v1"));
    ASSERT_TRUE(llvm::StringRef(path).ends_with("k1.pch"));
}

TEST_CASE(AbortRemovesTmp) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    auto pending = store.begin_store("pch", "k1");
    ASSERT_TRUE(fs::write(pending.tmp_path, "junk").has_value());
    auto tmp_path = pending.tmp_path;
    store.abort(pending);

    ASSERT_FALSE(llvm::sys::fs::exists(tmp_path));
    ASSERT_FALSE(store.lookup("pch", "k1").has_value());
}

TEST_CASE(CommitWithoutWriteFails) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    auto pending = store.begin_store("pch", "k1");
    ASSERT_FALSE(store.commit(std::move(pending)).has_value());
}

TEST_CASE(SurvivesReopen) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_lru(store);
        put(store, "pch", "k1", "persisted");
        store.shutdown();
    }

    auto store = open_store(tmp);
    register_lru(store);
    auto hit = store.lookup("pch", "k1");
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(fs::read(*hit).value_or(""), "persisted");
}

TEST_CASE(VersionBumpDiscards) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_lru(store);
        put(store, "pch", "k1", "old version blob");
        store.shutdown();
    }

    auto store = open_store(tmp, version + 1);
    register_lru(store);
    ASSERT_FALSE(store.lookup("pch", "k1").has_value());
    ASSERT_FALSE(llvm::sys::fs::exists(tmp.path("root/cache/v1")));
}

TEST_CASE(LegacyLayoutDiscarded) {
    TempDir tmp;
    // Pre-versioning layout: blobs and metadata directly under cache/.
    tmp.touch("root/cache/cache.json", "{}");
    tmp.touch("root/cache/pch/deadbeef.pch", "old");

    auto store = open_store(tmp);
    ASSERT_FALSE(llvm::sys::fs::exists(tmp.path("root/cache/cache.json")));
    ASSERT_FALSE(llvm::sys::fs::exists(tmp.path("root/cache/pch")));
}

#ifndef _WIN32
TEST_CASE(SymlinkNotFollowed) {
    TempDir tmp;
    tmp.touch("outside/keep.txt", "data");
    tmp.mkdir("root/cache");
    // A stale entry that is a symlink to data outside the cache root: the
    // version sweep must unlink it without recursing into the target.
    [[maybe_unused]] auto linked =
        ::symlink(tmp.path("outside").c_str(), tmp.path("root/cache/v0").c_str());

    auto store = open_store(tmp);
    ASSERT_FALSE(llvm::sys::fs::exists(tmp.path("root/cache/v0")));
    ASSERT_TRUE(llvm::sys::fs::exists(tmp.path("outside/keep.txt")));
}
#endif

TEST_CASE(LruEviction) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store, 25);  // fits two 10-byte blobs, not three

    put(store, "pch", "a", "aaaaaaaaaa");
    put(store, "pch", "b", "bbbbbbbbbb");

    // Touch "a" so "b" becomes the coldest entry.
    ASSERT_TRUE(store.lookup("pch", "a").has_value());

    put(store, "pch", "c", "cccccccccc");

    ASSERT_TRUE(store.lookup("pch", "a").has_value());
    ASSERT_FALSE(store.lookup("pch", "b").has_value());
    ASSERT_TRUE(store.lookup("pch", "c").has_value());
}

TEST_CASE(FreshCommitNotEvicted) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store, 5);  // smaller than a single blob

    auto path = put(store, "pch", "big", "0123456789");
    ASSERT_TRUE(llvm::sys::fs::exists(path));
    ASSERT_TRUE(store.lookup("pch", "big").has_value());
}

TEST_CASE(PersistentNeverEvicted) {
    TempDir tmp;
    auto store = open_store(tmp);
    store.register_namespace(
        {.name = "index", .extension = ".idx", .policy = CachePolicy::Persistent, .max_bytes = 1});

    put(store, "index", "a", "aaaaaaaaaa");
    put(store, "index", "b", "bbbbbbbbbb");

    ASSERT_TRUE(store.lookup("index", "a").has_value());
    ASSERT_TRUE(store.lookup("index", "b").has_value());
}

TEST_CASE(PersistentRewriteWins) {
    TempDir tmp;
    auto store = open_store(tmp);
    store.register_namespace(
        {.name = "index", .extension = ".idx", .policy = CachePolicy::Persistent});

    // Persistent keys are mutable: a rewrite of the same key must serve
    // the new content, never the old blob.
    put(store, "index", "project", "first snapshot");
    auto path = put(store, "index", "project", "second");
    ASSERT_EQ(fs::read(path).value_or(""), "second");
}

TEST_CASE(PersistentRenameRetry) {
    TempDir tmp;
    auto store = open_store(tmp);
    store.register_namespace(
        {.name = "index", .extension = ".idx", .policy = CachePolicy::Persistent});

    // Squat the blob path with an empty directory: the first rename fails,
    // and the store must remove the obstacle and publish the new blob.
    tmp.mkdir("root/cache/v1/index/project.idx");
    auto path = put(store, "index", "project", "fresh");
    ASSERT_EQ(fs::read(path).value_or(""), "fresh");
    ASSERT_TRUE(store.lookup("index", "project").has_value());
}

TEST_CASE(LruStaleBlobReplaced) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    // Even LRU keys are not fully content-addressed (dependency edits
    // change PCH content without changing the key input): on a rename
    // collision with DIFFERENT content the stale blob must be replaced,
    // never kept.  Squat the path to force the collision portably.
    tmp.mkdir("root/cache/v1/pch/k1.pch");
    auto path = put(store, "pch", "k1", "fresh");
    ASSERT_EQ(fs::read(path).value_or(""), "fresh");
    ASSERT_TRUE(store.lookup("pch", "k1").has_value());
}

TEST_CASE(PersistentCommitFailureSurfaces) {
    TempDir tmp;
    auto store = open_store(tmp);
    store.register_namespace(
        {.name = "index", .extension = ".idx", .policy = CachePolicy::Persistent});

    // A non-empty directory can neither be renamed over nor removed: the
    // commit must report the failure, not silently claim the data is stored.
    tmp.touch("root/cache/v1/index/project.idx/squatter", "x");
    auto pending = store.begin_store("index", "project");
    ASSERT_TRUE(fs::write(pending.tmp_path, "dropped").has_value());
    ASSERT_FALSE(store.commit(std::move(pending)).has_value());
    ASSERT_FALSE(store.lookup("index", "project").has_value());
}

TEST_CASE(InvalidateRemovesBlob) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    auto path = put(store, "pch", "k1", "blob");
    store.invalidate("pch", "k1");

    ASSERT_FALSE(store.lookup("pch", "k1").has_value());
    ASSERT_FALSE(llvm::sys::fs::exists(path));
}

TEST_CASE(ForEachKey) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    put(store, "pch", "a", "1");
    put(store, "pch", "b", "2");

    std::vector<std::string> keys;
    store.for_each_key("pch", [&](llvm::StringRef key) { keys.push_back(key.str()); });
    std::ranges::sort(keys);
    ASSERT_EQ(keys, (std::vector<std::string>{"a", "b"}));

    // Snapshot semantics: invalidating from the callback must be safe.
    store.for_each_key("pch", [&](llvm::StringRef key) { store.invalidate("pch", key); });
    ASSERT_FALSE(store.lookup("pch", "a").has_value());
    ASSERT_FALSE(store.lookup("pch", "b").has_value());
}

TEST_CASE(MissingManifestRescans) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_lru(store);
        put(store, "pch", "k1", "scanned blob");
        store.shutdown();
    }

    [[maybe_unused]] auto removed = llvm::sys::fs::remove(tmp.path("root/cache/v1/manifest.json"));
    ASSERT_FALSE(llvm::sys::fs::exists(tmp.path("root/cache/v1/manifest.json")));

    auto store = open_store(tmp);
    register_lru(store);
    auto hit = store.lookup("pch", "k1");
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(fs::read(*hit).value_or(""), "scanned blob");
}

TEST_CASE(CorruptManifestRescans) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_lru(store);
        put(store, "pch", "k1", "scanned blob");
        store.shutdown();
    }

    tmp.touch("root/cache/v1/manifest.json", "this is not json {{{");

    auto store = open_store(tmp);
    register_lru(store);
    ASSERT_TRUE(store.lookup("pch", "k1").has_value());
}

TEST_CASE(UncheckpointedBlobAdopted) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_lru(store);
        put(store, "pch", "k1", "checkpointed");
        store.shutdown();
    }

    // Simulate a crash after commit but before checkpoint: a blob exists
    // on disk that the manifest has never heard of.
    tmp.touch("root/cache/v1/pch/orphan.pch", "uncheckpointed");

    auto store = open_store(tmp);
    register_lru(store);
    ASSERT_TRUE(store.lookup("pch", "k1").has_value());
    ASSERT_TRUE(store.lookup("pch", "orphan").has_value());
}

TEST_CASE(DeadInstanceTmpSwept) {
    TempDir tmp;
    tmp.touch(std::string("root/cache/v1/tmp/") + dead_pid + "/0.pch", "leftover");

    auto store = open_store(tmp);
    ASSERT_FALSE(llvm::sys::fs::exists(tmp.path(std::string("root/cache/v1/tmp/") + dead_pid)));
}

TEST_CASE(ShutdownRemovesOwnTmp) {
    TempDir tmp;
    auto pid = std::to_string(llvm::sys::Process::getProcessId());
    {
        auto store = open_store(tmp);
        register_lru(store);
        ASSERT_TRUE(llvm::sys::fs::exists(tmp.path("root/cache/v1/tmp/" + pid)));
        store.shutdown();
    }
    ASSERT_FALSE(llvm::sys::fs::exists(tmp.path("root/cache/v1/tmp/" + pid)));
}

TEST_CASE(ScratchBasics) {
    TempDir tmp;
    auto store = open_store(tmp);
    store.register_namespace(
        {.name = "header_context", .extension = ".h", .policy = CachePolicy::Scratch});

    auto pid = std::to_string(llvm::sys::Process::getProcessId());
    auto path = put(store, "header_context", "k1", "preamble");

    // Scratch blobs live under this instance's pid directory.
    ASSERT_TRUE(llvm::StringRef(path).contains(pid));
    ASSERT_TRUE(store.lookup("header_context", "k1").has_value());

    // Scratch entries never enter the manifest.
    store.checkpoint();
    auto manifest = fs::read(tmp.path("root/cache/v1/manifest.json"));
    if(manifest.has_value()) {
        ASSERT_FALSE(llvm::StringRef(*manifest).contains("header_context"));
    }

    // shutdown removes the whole instance directory.
    store.shutdown();
    ASSERT_FALSE(llvm::sys::fs::exists(path));
}

TEST_CASE(ScratchDeadPidSwept) {
    TempDir tmp;
    tmp.touch(std::string("root/cache/v1/header_context/") + dead_pid + "/x.h", "stale");

    auto store = open_store(tmp);
    store.register_namespace(
        {.name = "header_context", .extension = ".h", .policy = CachePolicy::Scratch});

    ASSERT_FALSE(
        llvm::sys::fs::exists(tmp.path(std::string("root/cache/v1/header_context/") + dead_pid)));
}

TEST_CASE(CommitOverwriteSameKey) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store, 20);

    put(store, "pch", "k1", "first");
    auto path = put(store, "pch", "k1", "second");
    ASSERT_EQ(fs::read(path).value_or(""), "second");

    // total_size must account for replacement, not accumulate: correct
    // accounting gives 6 + 10 = 16 <= 20 (no eviction); accumulating the
    // replaced 5 bytes would give 21 > 20 and evict k1.
    put(store, "pch", "x", "xxxxxxxxxx");
    ASSERT_TRUE(store.lookup("pch", "k1").has_value());
    ASSERT_TRUE(store.lookup("pch", "x").has_value());
}

TEST_CASE(ManifestAtimePersisted) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_lru(store);
        put(store, "pch", "a", "aaaaaaaaaa");
        put(store, "pch", "b", "bbbbbbbbbb");
        // Touch "a" after both writes so only the manifest knows it is the
        // hotter entry — the mtime fallback would conclude the opposite.
        ASSERT_TRUE(store.lookup("pch", "a").has_value());
        store.shutdown();
    }

    // Reopen with a budget that forces one eviction at registration.
    auto store = open_store(tmp);
    register_lru(store, 15);
    ASSERT_TRUE(store.lookup("pch", "a").has_value());
    ASSERT_FALSE(store.lookup("pch", "b").has_value());
}

TEST_CASE(CheckpointAutoTriggers) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    // Enough commits to cross the internal change threshold; the manifest
    // must appear without an explicit checkpoint() or shutdown().
    for(int i = 0; i < 16; ++i) {
        put(store, "pch", std::format("k{}", i), "blob");
    }
    auto manifest = fs::read(tmp.path("root/cache/v1/manifest.json"));
    ASSERT_TRUE(manifest.has_value());
    ASSERT_TRUE(llvm::StringRef(*manifest).contains("k0"));
}

TEST_CASE(PairStoreAndLookup) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_paired(store);

    // A primary alone is an incomplete pair: lookup serves it, lookup_aux
    // must miss.
    put(store, "pch", "k1", "primary blob");
    ASSERT_TRUE(store.lookup("pch", "k1").has_value());
    ASSERT_FALSE(store.lookup_aux("pch", "k1").has_value());

    auto aux_path = put_aux(store, "pch", "k1", "aux blob");
    ASSERT_TRUE(llvm::StringRef(aux_path).ends_with(".pch.idx"));

    auto hit = store.lookup_aux("pch", "k1");
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(fs::read(*hit).value_or(""), "aux blob");
}

TEST_CASE(AuxWithoutPrimaryFails) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_paired(store);

    auto pending = store.begin_store_aux("pch", "ghost");
    require(fs::write(pending.tmp_path, "orphan").has_value(), "tmp write failed");
    ASSERT_FALSE(store.commit(std::move(pending)).has_value());
    ASSERT_FALSE(store.lookup_aux("pch", "ghost").has_value());
}

TEST_CASE(PrimaryRecommitResetsAux) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_paired(store);

    put(store, "pch", "k1", "old primary");
    auto aux_path = put_aux(store, "pch", "k1", "old aux");

    // Republishing the primary must drop the stale aux: yesterday's aux
    // next to today's primary would be a silent mismatch.
    put(store, "pch", "k1", "new primary");
    ASSERT_FALSE(store.lookup_aux("pch", "k1").has_value());
    ASSERT_FALSE(fs::read(aux_path).has_value());

    put_aux(store, "pch", "k1", "new aux");
    ASSERT_TRUE(store.lookup_aux("pch", "k1").has_value());
}

TEST_CASE(PairEvictedTogether) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_paired(store, 25);

    put(store, "pch", "k1", "aaaaaaaaaa");
    auto aux_path = put_aux(store, "pch", "k1", "aaaaaaaaaa");

    // The pair counts as one 20-byte entry; the next 10-byte pair pushes
    // the total over budget and k1 must vanish whole — both files.
    put(store, "pch", "k2", "bbbbb");
    put_aux(store, "pch", "k2", "bbbbb");

    ASSERT_FALSE(store.lookup("pch", "k1").has_value());
    ASSERT_FALSE(store.lookup_aux("pch", "k1").has_value());
    ASSERT_FALSE(fs::read(aux_path).has_value());
    ASSERT_TRUE(store.lookup("pch", "k2").has_value());
    ASSERT_TRUE(store.lookup_aux("pch", "k2").has_value());
}

TEST_CASE(PairSurvivesReopen) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_paired(store);
        put(store, "pch", "k1", "primary");
        put_aux(store, "pch", "k1", "aux");
        store.shutdown();
    }

    auto store = open_store(tmp);
    register_paired(store);
    ASSERT_TRUE(store.lookup("pch", "k1").has_value());
    auto hit = store.lookup_aux("pch", "k1");
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(fs::read(*hit).value_or(""), "aux");
}

TEST_CASE(StaleAuxDropped) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_paired(store);
        put(store, "pch", "k1", "primary");
        put_aux(store, "pch", "k1", "aux");
        store.shutdown();
    }

    // Residue of an aux removal that failed while the file was held open
    // (see reset_aux_locked): the primary was republished, the old aux
    // stayed behind. Pairs commit primary-first, so a legitimate aux is
    // never older than its primary — registration must not adopt this one.
    // int-FD flavor: setLastAccessAndModificationTime has no overload for
    // the native handle type on Windows.
    auto aux_path = tmp.path("root/cache/v1/pch/k1.pch.idx");
    int fd = 0;
    ASSERT_FALSE(bool(llvm::sys::fs::openFileForWrite(aux_path,
                                                      fd,
                                                      llvm::sys::fs::CD_OpenExisting,
                                                      llvm::sys::fs::OF_None)));
    auto old_time = std::chrono::system_clock::now() - std::chrono::hours(1);
    ASSERT_FALSE(bool(llvm::sys::fs::setLastAccessAndModificationTime(fd, old_time, old_time)));
    llvm::sys::Process::SafelyCloseFileDescriptor(fd);

    auto store = open_store(tmp);
    register_paired(store);
    ASSERT_TRUE(store.lookup("pch", "k1").has_value());
    ASSERT_FALSE(store.lookup_aux("pch", "k1").has_value());
    ASSERT_FALSE(fs::read(aux_path).has_value());
}

TEST_CASE(OrphanAuxSwept) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_paired(store);
        store.shutdown();
    }

    // Crash residue: an aux blob whose primary is gone.  Registration must
    // remove it — nothing can ever reference it again.
    tmp.touch("root/cache/v1/pch/ghost.pch.idx", "orphan");

    auto store = open_store(tmp);
    register_paired(store);
    ASSERT_FALSE(store.lookup_aux("pch", "ghost").has_value());
    ASSERT_FALSE(fs::read(tmp.path("root/cache/v1/pch/ghost.pch.idx")).has_value());
}

TEST_CASE(InvalidateRemovesPair) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_paired(store);

    put(store, "pch", "k1", "primary");
    auto aux_path = put_aux(store, "pch", "k1", "aux");

    store.invalidate("pch", "k1");
    ASSERT_FALSE(store.lookup("pch", "k1").has_value());
    ASSERT_FALSE(store.lookup_aux("pch", "k1").has_value());
    ASSERT_FALSE(fs::read(aux_path).has_value());
}

};  // TEST_SUITE(CacheStore)

}  // namespace

}  // namespace clice::testing
