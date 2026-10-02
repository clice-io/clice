#include <cstdlib>
#include <format>
#include <print>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "test/temp_dir.h"
#include "test/test.h"
#include "support/cache_store.h"

#include "llvm/Support/MemoryBuffer.h"
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
    require(!vfs::write(pending.tmp_path, content), "tmp write failed");
    auto committed = store.commit(std::move(pending));
    require(committed.has_value(), "aux commit failed");
    return *committed;
}

/// Run a full two-phase write with the given content.
std::string
    put(CacheStore& store, llvm::StringRef ns, llvm::StringRef key, llvm::StringRef content) {
    auto pending = store.begin_store(ns, key);
    require(!pending.tmp_path.empty(), "begin_store returned no tmp path");
    require(!vfs::write(pending.tmp_path, content), "tmp write failed");
    auto committed = store.commit(std::move(pending));
    require(committed.has_value(), "commit failed");
    return *committed;
}

ZEST_SUITE(CacheStore){

    ZEST_CASE(StoreAndLookup){TempDir tmp;
auto store = open_store(tmp);
register_lru(store);

ASSERT(!store.lookup("pch", "k1").has_value());

auto path = put(store, "pch", "k1", "blob content");

auto hit = store.lookup("pch", "k1");
ASSERT(hit);
ASSERT(*hit == path);
ASSERT(read_file(*hit).value_or("") == "blob content");

// The blob landed inside the versioned namespace directory.
ASSERT(llvm::StringRef(path).contains("v1"));
ASSERT(llvm::StringRef(path).ends_with("k1.pch"));

}  // namespace

ZEST_CASE(RootIgnoreMarkers) {
    TempDir tmp;
    auto store = open_store(tmp);

    // Opening alone marks nothing: the root may be a user-configured,
    // shared directory.
    ASSERT(!llvm::sys::fs::exists(tmp.path("root/.gitignore")));

    CacheStore::write_ignore_markers(tmp.path("root"));

    // config.toml stays visible: the root doubles as .clice/config.toml.
    ASSERT(read_file(tmp.path("root/.gitignore")).value_or("") == "*\n!config.toml\n");
    auto tag = read_file(tmp.path("root/CACHEDIR.TAG")).value_or("");
    ASSERT(llvm::StringRef(tag).starts_with("Signature: 8a477f597d28d172789f06886806bc55"));
}

ZEST_CASE(MarkersCreateRoot) {
    TempDir tmp;

    // Sessions mark the defaulted root before anything else creates it.
    CacheStore::write_ignore_markers(tmp.path("fresh"));

    ASSERT(read_file(tmp.path("fresh/.gitignore")).value_or("") == "*\n!config.toml\n");
    ASSERT(llvm::sys::fs::exists(tmp.path("fresh/CACHEDIR.TAG")));
}

ZEST_CASE(IgnoreMarkersPreserved) {
    TempDir tmp;
    { auto store = open_store(tmp); }
    require(!vfs::write(tmp.path("root/.gitignore"), "custom\n"), "rewrite failed");

    CacheStore::write_ignore_markers(tmp.path("root"));
    ASSERT(read_file(tmp.path("root/.gitignore")).value_or("") == "custom\n");
    ASSERT(llvm::sys::fs::exists(tmp.path("root/CACHEDIR.TAG")));
}

ZEST_CASE(DropRemovesTmp) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    // Self-cleaning is the abort: an entry destroyed without being
    // committed removes its tmp file — including when a cancellation
    // destroys the owning coroutine frame without resuming it.
    std::string tmp_path;
    {
        auto pending = store.begin_store("pch", "k1");
        ASSERT(!vfs::write(pending.tmp_path, "junk"));
        tmp_path = pending.tmp_path;
    }

    ASSERT(!llvm::sys::fs::exists(tmp_path));
    ASSERT(!store.lookup("pch", "k1").has_value());

    // A moved-from entry no longer owns the tmp file.
    auto pending = store.begin_store("pch", "k2");
    ASSERT(!vfs::write(pending.tmp_path, "junk"));
    auto second = pending.tmp_path;
    {
        auto moved = std::move(pending);
        ASSERT(llvm::sys::fs::exists(second));
    }
    ASSERT(!llvm::sys::fs::exists(second));

    // Move assignment cleans the destination's own tmp before adopting.
    auto lhs = store.begin_store("pch", "k3");
    auto rhs = store.begin_store("pch", "k4");
    ASSERT(!vfs::write(lhs.tmp_path, "junk"));
    ASSERT(!vfs::write(rhs.tmp_path, "junk"));
    auto third = lhs.tmp_path;
    auto fourth = rhs.tmp_path;
    lhs = std::move(rhs);
    ASSERT(!llvm::sys::fs::exists(third));
    ASSERT(llvm::sys::fs::exists(fourth));
}

ZEST_CASE(CommitWithoutWriteFails) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    auto pending = store.begin_store("pch", "k1");
    ASSERT(!store.commit(std::move(pending)).has_value());
}

ZEST_CASE(SurvivesReopen) {
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
    ASSERT(hit);
    ASSERT(read_file(*hit).value_or("") == "persisted");
}

ZEST_CASE(VersionBumpDiscards) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_lru(store);
        put(store, "pch", "k1", "old version blob");
        store.shutdown();
    }

    auto store = open_store(tmp, version + 1);
    register_lru(store);
    ASSERT(!store.lookup("pch", "k1").has_value());
    ASSERT(!llvm::sys::fs::exists(tmp.path("root/cache/v1")));
}

ZEST_CASE(LiveLayoutKept) {
    TempDir tmp;
    // A clice of another version is live inside v1 (our own pid stands in
    // for its `tmp/{pid}` marker): the sweep must not delete the cache out
    // from under it.
    auto self = std::to_string(llvm::sys::Process::getProcessId());
    tmp.touch("root/cache/v1/tmp/" + self + "/0.pch");
    tmp.touch("root/cache/v1/pch/k1.pch", "live");

    auto store = open_store(tmp, version + 1);
    ASSERT(llvm::sys::fs::exists(tmp.path("root/cache/v1/pch/k1.pch")));
}

ZEST_CASE(CrashedLayoutDiscarded) {
    TempDir tmp;
    // A crashed old-version instance leaves its `tmp/{pid}` behind; a dead
    // pid does not hold the layout alive.
    tmp.touch(std::string("root/cache/v1/tmp/") + dead_pid + "/0.pch");

    auto store = open_store(tmp, version + 1);
    ASSERT(!llvm::sys::fs::exists(tmp.path("root/cache/v1")));
}

ZEST_CASE(LegacyLayoutDiscarded) {
    TempDir tmp;
    // Pre-versioning layout: blobs and metadata directly under cache/.
    tmp.touch("root/cache/cache.json", "{}");
    tmp.touch("root/cache/pch/deadbeef.pch", "old");

    auto store = open_store(tmp);
    ASSERT(!llvm::sys::fs::exists(tmp.path("root/cache/cache.json")));
    ASSERT(!llvm::sys::fs::exists(tmp.path("root/cache/pch")));
}

#ifndef _WIN32
ZEST_CASE(SymlinkNotFollowed) {
    TempDir tmp;
    tmp.touch("outside/keep.txt", "data");
    tmp.mkdir("root/cache");
    // A stale entry that is a symlink to data outside the cache root: the
    // version sweep must unlink it without recursing into the target.
    [[maybe_unused]] auto linked =
        ::symlink(tmp.path("outside").c_str(), tmp.path("root/cache/v0").c_str());

    auto store = open_store(tmp);
    ASSERT(!llvm::sys::fs::exists(tmp.path("root/cache/v0")));
    ASSERT(llvm::sys::fs::exists(tmp.path("outside/keep.txt")));
}
#endif

ZEST_CASE(LruEviction) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store, 25);  // fits two 10-byte blobs, not three

    put(store, "pch", "a", "aaaaaaaaaa");
    put(store, "pch", "b", "bbbbbbbbbb");

    // Touch "a" so "b" becomes the coldest entry.
    ASSERT(store.lookup("pch", "a"));

    put(store, "pch", "c", "cccccccccc");

    ASSERT(store.lookup("pch", "a"));
    ASSERT(!store.lookup("pch", "b").has_value());
    ASSERT(store.lookup("pch", "c"));

    // The eviction is reported exactly once: owners of derived in-memory
    // state drain the record on their own loop, and a second drain must
    // not replay it.
    auto evicted = store.take_evictions();
    ASSERT(evicted.size() == 1U);
    ASSERT(evicted[0].ns == std::string("pch"));
    ASSERT(evicted[0].key == std::string("b"));
    ASSERT(store.take_evictions().empty());
}

ZEST_CASE(FreshCommitNotEvicted) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store, 5);  // smaller than a single blob

    auto path = put(store, "pch", "big", "0123456789");
    ASSERT(llvm::sys::fs::exists(path));
    ASSERT(store.lookup("pch", "big"));
}

ZEST_CASE(RewriteWins) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    // Keys are mutable (not fully content-addressed): a rewrite of the
    // same key must serve the new content, never the old blob.
    put(store, "pch", "k1", "first snapshot");
    auto path = put(store, "pch", "k1", "second");
    ASSERT(read_file(path).value_or("") == "second");
}

ZEST_CASE(LruStaleBlobReplaced) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    // Even LRU keys are not fully content-addressed (dependency edits
    // change PCH content without changing the key input): on a rename
    // collision with DIFFERENT content the stale blob must be replaced,
    // never kept.  Squat the path to force the collision portably.
    tmp.mkdir("root/cache/v1/pch/k1.pch");
    auto path = put(store, "pch", "k1", "fresh");
    ASSERT(read_file(path).value_or("") == "fresh");
    ASSERT(store.lookup("pch", "k1"));
}

ZEST_CASE(CommitFailureSurfaces) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    // A non-empty directory can neither be renamed over nor removed: the
    // commit must report the failure, not silently claim the data is stored.
    tmp.touch("root/cache/v1/pch/k1.pch/squatter", "x");
    auto pending = store.begin_store("pch", "k1");
    ASSERT(!vfs::write(pending.tmp_path, "dropped"));
    ASSERT(!store.commit(std::move(pending)).has_value());
    ASSERT(!store.lookup("pch", "k1").has_value());
}

ZEST_CASE(InvalidateRemovesBlob) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    auto path = put(store, "pch", "k1", "blob");
    store.invalidate("pch", "k1");

    ASSERT(!store.lookup("pch", "k1").has_value());
    ASSERT(!llvm::sys::fs::exists(path));
}

ZEST_CASE(InvalidateMappedBlob) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    auto path = put(store, "pch", "k1", std::string(64 * 1024, 'x'));
    auto mapped = llvm::MemoryBuffer::getFile(path,
                                              /*IsText=*/false,
                                              /*RequiresNullTerminator=*/false);
    ASSERT(bool(mapped));
    ASSERT((*mapped)->getBufferKind() == llvm::MemoryBuffer::MemoryBuffer_MMap);

    store.invalidate("pch", "k1");

    ASSERT(!llvm::sys::fs::exists(path));
    ASSERT((*mapped)->getBuffer().back() == 'x');
}

ZEST_CASE(MissingManifestRescans) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_lru(store);
        put(store, "pch", "k1", "scanned blob");
        store.shutdown();
    }

    [[maybe_unused]] auto removed = llvm::sys::fs::remove(tmp.path("root/cache/v1/manifest.json"));
    ASSERT(!llvm::sys::fs::exists(tmp.path("root/cache/v1/manifest.json")));

    auto store = open_store(tmp);
    register_lru(store);
    auto hit = store.lookup("pch", "k1");
    ASSERT(hit);
    ASSERT(read_file(*hit).value_or("") == "scanned blob");
}

ZEST_CASE(CorruptManifestRescans) {
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
    ASSERT(store.lookup("pch", "k1"));
}

ZEST_CASE(UncheckpointedBlobAdopted) {
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
    ASSERT(store.lookup("pch", "k1"));
    ASSERT(store.lookup("pch", "orphan"));
}

ZEST_CASE(DeadInstanceTmpSwept) {
    TempDir tmp;
    tmp.touch(std::string("root/cache/v1/tmp/") + dead_pid + "/0.pch", "leftover");

    auto store = open_store(tmp);
    ASSERT(!llvm::sys::fs::exists(tmp.path(std::string("root/cache/v1/tmp/") + dead_pid)));
}

ZEST_CASE(ShutdownRemovesOwnTmp) {
    TempDir tmp;
    auto pid = std::to_string(llvm::sys::Process::getProcessId());
    {
        auto store = open_store(tmp);
        register_lru(store);
        ASSERT(llvm::sys::fs::exists(tmp.path("root/cache/v1/tmp/" + pid)));
        store.shutdown();
    }
    ASSERT(!llvm::sys::fs::exists(tmp.path("root/cache/v1/tmp/" + pid)));
}

ZEST_CASE(ScratchBasics) {
    TempDir tmp;
    auto store = open_store(tmp);
    store.register_namespace(
        {.name = "header_context", .extension = ".h", .policy = CachePolicy::Scratch});

    auto pid = std::to_string(llvm::sys::Process::getProcessId());
    auto path = put(store, "header_context", "k1", "preamble");

    // Scratch blobs live under this instance's pid directory.
    ASSERT(llvm::StringRef(path).contains(pid));
    ASSERT(store.lookup("header_context", "k1"));

    // Scratch entries never enter the manifest.
    store.checkpoint();
    auto manifest = read_file(tmp.path("root/cache/v1/manifest.json"));
    if(manifest.has_value()) {
        ASSERT(!llvm::StringRef(*manifest).contains("header_context"));
    }

    // shutdown removes the whole instance directory.
    store.shutdown();
    ASSERT(!llvm::sys::fs::exists(path));
}

ZEST_CASE(ScratchDeadPidSwept) {
    TempDir tmp;
    tmp.touch(std::string("root/cache/v1/header_context/") + dead_pid + "/x.h", "stale");

    auto store = open_store(tmp);
    store.register_namespace(
        {.name = "header_context", .extension = ".h", .policy = CachePolicy::Scratch});

    ASSERT(
        !llvm::sys::fs::exists(tmp.path(std::string("root/cache/v1/header_context/") + dead_pid)));
}

ZEST_CASE(CommitOverwriteSameKey) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store, 20);

    put(store, "pch", "k1", "first");
    auto path = put(store, "pch", "k1", "second");
    ASSERT(read_file(path).value_or("") == "second");

    // total_size must account for replacement, not accumulate: correct
    // accounting gives 6 + 10 = 16 <= 20 (no eviction); accumulating the
    // replaced 5 bytes would give 21 > 20 and evict k1.
    put(store, "pch", "x", "xxxxxxxxxx");
    ASSERT(store.lookup("pch", "k1"));
    ASSERT(store.lookup("pch", "x"));
}

ZEST_CASE(ManifestAtimePersisted) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_lru(store);
        put(store, "pch", "a", "aaaaaaaaaa");
        put(store, "pch", "b", "bbbbbbbbbb");
        // Touch "a" after both writes so only the manifest knows it is the
        // hotter entry — the mtime fallback would conclude the opposite.
        ASSERT(store.lookup("pch", "a"));
        store.shutdown();
    }

    // Reopen with a budget that forces one eviction at registration.
    auto store = open_store(tmp);
    register_lru(store, 15);
    ASSERT(store.lookup("pch", "a"));
    ASSERT(!store.lookup("pch", "b").has_value());
}

ZEST_CASE(CheckpointAutoTriggers) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_lru(store);

    // Enough commits to cross the internal change threshold; the manifest
    // must appear without an explicit checkpoint() or shutdown().
    for(int i = 0; i < 16; ++i) {
        put(store, "pch", std::format("k{}", i), "blob");
    }
    auto manifest = read_file(tmp.path("root/cache/v1/manifest.json"));
    ASSERT(manifest);
    ASSERT(llvm::StringRef(*manifest).contains("k0"));
}

ZEST_CASE(PairStoreAndLookup) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_paired(store);

    // A primary alone is an incomplete pair: lookup serves it, lookup_aux
    // must miss.
    put(store, "pch", "k1", "primary blob");
    ASSERT(store.lookup("pch", "k1"));
    ASSERT(!store.lookup_aux("pch", "k1").has_value());

    auto aux_path = put_aux(store, "pch", "k1", "aux blob");
    ASSERT(llvm::StringRef(aux_path).ends_with(".pch.idx"));

    auto hit = store.lookup_aux("pch", "k1");
    ASSERT(hit);
    ASSERT(read_file(*hit).value_or("") == "aux blob");
}

ZEST_CASE(AuxWithoutPrimaryFails) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_paired(store);

    auto pending = store.begin_store_aux("pch", "ghost");
    require(!vfs::write(pending.tmp_path, "orphan"), "tmp write failed");
    ASSERT(!store.commit(std::move(pending)).has_value());
    ASSERT(!store.lookup_aux("pch", "ghost").has_value());
}

ZEST_CASE(PrimaryRecommitResetsAux) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_paired(store);

    put(store, "pch", "k1", "old primary");
    auto aux_path = put_aux(store, "pch", "k1", "old aux");

    // Republishing the primary must drop the stale aux: yesterday's aux
    // next to today's primary would be a silent mismatch.
    put(store, "pch", "k1", "new primary");
    ASSERT(!store.lookup_aux("pch", "k1").has_value());
    ASSERT(!read_file(aux_path).has_value());

    put_aux(store, "pch", "k1", "new aux");
    ASSERT(store.lookup_aux("pch", "k1"));
}

ZEST_CASE(PairEvictedTogether) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_paired(store, 25);

    put(store, "pch", "k1", "aaaaaaaaaa");
    auto aux_path = put_aux(store, "pch", "k1", "aaaaaaaaaa");

    // The pair counts as one 20-byte entry; the next 10-byte pair pushes
    // the total over budget and k1 must vanish whole — both files.
    put(store, "pch", "k2", "bbbbb");
    put_aux(store, "pch", "k2", "bbbbb");

    ASSERT(!store.lookup("pch", "k1").has_value());
    ASSERT(!store.lookup_aux("pch", "k1").has_value());
    ASSERT(!read_file(aux_path).has_value());
    ASSERT(store.lookup("pch", "k2"));
    ASSERT(store.lookup_aux("pch", "k2"));
}

ZEST_CASE(PairSurvivesReopen) {
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
    ASSERT(store.lookup("pch", "k1"));
    auto hit = store.lookup_aux("pch", "k1");
    ASSERT(hit);
    ASSERT(read_file(*hit).value_or("") == "aux");
}

ZEST_CASE(StaleAuxDropped) {
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
    ASSERT(!bool(llvm::sys::fs::openFileForWrite(aux_path,
                                                 fd,
                                                 llvm::sys::fs::CD_OpenExisting,
                                                 llvm::sys::fs::OF_None)));
    auto old_time = std::chrono::system_clock::now() - std::chrono::hours(1);
    ASSERT(!bool(llvm::sys::fs::setLastAccessAndModificationTime(fd, old_time, old_time)));
    llvm::sys::Process::SafelyCloseFileDescriptor(fd);

    auto store = open_store(tmp);
    register_paired(store);
    ASSERT(store.lookup("pch", "k1"));
    ASSERT(!store.lookup_aux("pch", "k1").has_value());
    ASSERT(!read_file(aux_path).has_value());
}

ZEST_CASE(OrphanAuxSwept) {
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
    ASSERT(!store.lookup_aux("pch", "ghost").has_value());
    ASSERT(!read_file(tmp.path("root/cache/v1/pch/ghost.pch.idx")).has_value());
}

ZEST_CASE(InvalidateRemovesPair) {
    TempDir tmp;
    auto store = open_store(tmp);
    register_paired(store);

    put(store, "pch", "k1", "primary");
    auto aux_path = put_aux(store, "pch", "k1", "aux");

    store.invalidate("pch", "k1");
    ASSERT(!store.lookup("pch", "k1").has_value());
    ASSERT(!store.lookup_aux("pch", "k1").has_value());
    ASSERT(!read_file(aux_path).has_value());
}

ZEST_CASE(ReadOnlyOpenRequiresStore) {
    TempDir tmp;
    // The parent directory alone (config resolution creates it eagerly)
    // must not pass for an existing store.
    tmp.touch("root/cache/marker", "");
    auto store = CacheStore::open(tmp.path("root"), version, /*read_only=*/true);
    ASSERT(!store.has_value());
    ASSERT(store.error() == std::errc::no_such_file_or_directory);
}

ZEST_CASE(ReadOnlyOpenTouchesNothing) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_lru(store);
        put(store, "pch", "k1", "blob");
        store.shutdown();
    }

    // A newer-version inspector must neither create its own directory nor
    // sweep the older version a live server may still be using.
    auto store = CacheStore::open(tmp.path("root"), version + 1, /*read_only=*/true);
    ASSERT(!store.has_value());
    ASSERT(!llvm::sys::fs::exists(tmp.path("root/cache/v2")));

    auto reader = CacheStore::open(tmp.path("root"), version, /*read_only=*/true);
    ASSERT(reader);
    register_lru(*reader);
    ASSERT(reader->lookup("pch", "k1"));
    auto pid = std::to_string(llvm::sys::Process::getProcessId());
    ASSERT(!llvm::sys::fs::exists(tmp.path("root/cache/v1/tmp/" + pid)));
}

ZEST_CASE(ReadOnlyNeverWrites) {
    TempDir tmp;
    {
        auto store = open_store(tmp);
        register_lru(store);
        put(store, "pch", "k1", "blob");
        store.shutdown();
    }
    auto manifest_before = read_file(tmp.path("root/cache/v1/manifest.json"));
    ASSERT(manifest_before);

    auto reader = CacheStore::open(tmp.path("root"), version, /*read_only=*/true);
    ASSERT(reader);
    // A budget below the blob size must not evict at registration, an
    // invalidate must not delete the live server's blob, and the atime
    // bumps from lookups must not publish a manifest at shutdown — with
    // no tmp dir it would even be staged in the working directory.
    register_lru(*reader, 1);
    ASSERT(reader->lookup("pch", "k1"));
    reader->invalidate("pch", "k1");
    reader->shutdown();

    ASSERT(read_file(tmp.path("root/cache/v1/pch/k1.pch")).value_or("") == "blob");
    ASSERT(read_file(tmp.path("root/cache/v1/manifest.json")).value_or("") == *manifest_before);
}

};  // namespace clice::testing

}  // namespace

}  // namespace clice::testing
