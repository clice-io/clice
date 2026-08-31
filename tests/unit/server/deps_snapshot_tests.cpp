#include "test/temp_dir.h"
#include "test/test.h"
#include "sched/workspace.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {
namespace {

/// A build_at (milliseconds since epoch, like the worker's `unit.build_at()`)
/// far enough in the future that every existing file clears the mtime guard
/// and earns a stat fast path at capture.
std::int64_t generous_build_at() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count() +
           10'000;
}

/// The consumed hash a worker would report for the file's current bytes
/// (0 = unreadable, which no test here expects).
std::uint64_t consumed_hash(llvm::StringRef path) {
    auto buf = llvm::MemoryBuffer::getFile(path);
    return buf ? llvm::xxh3_64bits((*buf)->getBuffer()) : 0;
}

/// Rewind a file's mtime out of the mtime-granularity guard window, the
/// way real project files predate a server start. A freshly touched file
/// is deliberately untrusted (see read_file_observed), so tests exercising
/// stamps and repairs must age their files first.
void age_file(llvm::StringRef path) {
    set_file_mtime(path, file_mtime_ns(path) - 10'000'000'000);
}

/// The shared stat fast path of the version a dep names (0 = none).
std::int64_t stamp_of(FileTable& pool, const DepState& dep) {
    return pool.version(pool.intern_version(dep.path_id, dep.hash)).mtime_ns;
}

bool changed(FileTable& pool, const DepsSnapshot& snap) {
    pool.begin_wave();
    return deps_changed(pool, snap);
}

TEST_SUITE(DepsSnapshot) {

TEST_CASE(FreshWhenUntouched) {
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");
    age_file(dep);

    FileTable pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());
    ASSERT_EQ(snap.deps.size(), 1u);
    ASSERT_FALSE(changed(pool, snap));

    // The passing check earned the version its stat fast path.
    ASSERT_EQ(stamp_of(pool, snap.deps[0]), file_mtime_ns(dep));
}

TEST_CASE(ScanPairStampsAtCapture) {
    // With the startup scan's read in the shared pair, the capture-time
    // stat is corroborated and the fast path exists before any check.
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");
    age_file(dep);

    FileTable pool;
    pool.read(pool.intern(dep));
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());
    ASSERT_EQ(stamp_of(pool, snap.deps[0]), file_mtime_ns(dep));
    ASSERT_FALSE(changed(pool, snap));
}

TEST_CASE(SnapshotsShareOneVersion) {
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");
    age_file(dep);

    FileTable pool;
    auto build_at = generous_build_at();
    auto first = capture_deps_snapshot(pool, {DepFile{dep, consumed_hash(dep)}}, build_at);
    auto second = capture_deps_snapshot(pool, {DepFile{dep, consumed_hash(dep)}}, build_at);
    ASSERT_EQ(pool.versions.size(), 1u);

    // A repair through one snapshot's check serves the other.
    ASSERT_FALSE(changed(pool, first));
    ASSERT_EQ(stamp_of(pool, second.deps[0]), file_mtime_ns(dep));
}

TEST_CASE(ImmediateEditDetected) {
    // The F44 shape: the dependency is saved right after the artifact's
    // freshness was captured — no watermark may bless it.
    TempDir tmp;
    tmp.touch("dep.h", "int value();\n");
    auto dep = tmp.path("dep.h");

    FileTable pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());
    tmp.touch("dep.h", "int renamed();\n");
    ASSERT_TRUE(changed(pool, snap));
}

TEST_CASE(BackdatedEditDetected) {
    // The F04 shape: the edit lands with an mtime that does not move
    // forward (rsync -t, git-restore-mtime). Equality comparison sends it
    // to the hash layer regardless of the timestamp's direction.
    TempDir tmp;
    tmp.touch("dep.h", "int old_name();\n");
    auto dep = tmp.path("dep.h");
    age_file(dep);

    FileTable pool;
    pool.read(pool.intern(dep));
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());
    auto recorded_mtime = stamp_of(pool, snap.deps[0]);
    ASSERT_TRUE(recorded_mtime != 0);

    tmp.touch("dep.h", "int new_name();\n");  // same length
    set_file_mtime(dep, recorded_mtime - 5'000'000'000);
    ASSERT_TRUE(changed(pool, snap));
}

TEST_CASE(TouchRepairsFastPath) {
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");
    age_file(dep);

    FileTable pool;
    pool.read(pool.intern(dep));
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());

    // Rewrite identical bytes: the stat moves, the content does not.
    tmp.touch("dep.h", "int f();\n");
    set_file_mtime(dep, stamp_of(pool, snap.deps[0]) + 5'000'000'000);
    ASSERT_FALSE(changed(pool, snap));

    // The passing hash comparison repaired the fast path in place.
    ASSERT_EQ(stamp_of(pool, snap.deps[0]), file_mtime_ns(dep));
}

TEST_CASE(PoisonedCaptureDetected) {
    // The F01 shape: the dependency changed between the build reading it
    // and the snapshot being captured. The consumed hash describes v1, the
    // disk holds v2, and the capture-time stat must not bless v2.
    TempDir tmp;
    tmp.touch("dep.h", "int v1();\n");
    auto dep = tmp.path("dep.h");
    auto consumed = consumed_hash(dep);

    tmp.touch("dep.h", "int v2();\n");
    // build_at in the past: the file's mtime falls inside "modified during
    // or after the build", so no fast path is recorded.
    FileTable pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed}
    },
                                      /*build_at=*/1);
    ASSERT_EQ(stamp_of(pool, snap.deps[0]), 0);
    ASSERT_TRUE(changed(pool, snap));
}

TEST_CASE(StaleScanPairCannotStamp) {
    // The pair describes bytes the scan read; after an edit the capture's
    // live stat no longer matches the pair, so the stale hash cannot
    // corroborate a stamp for the newly consumed version.
    TempDir tmp;
    tmp.touch("dep.h", "int v1();\n");
    auto dep = tmp.path("dep.h");

    FileTable pool;
    age_file(dep);
    pool.read(pool.intern(dep));

    tmp.touch("dep.h", "int v2();\n");
    age_file(dep);
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());
    ASSERT_EQ(stamp_of(pool, snap.deps[0]), 0);

    // The first check reads, proves the consumed bytes and repairs.
    ASSERT_FALSE(changed(pool, snap));
    ASSERT_EQ(stamp_of(pool, snap.deps[0]), file_mtime_ns(dep));
}

TEST_CASE(NoBaselineConverges) {
    // Capture during the guard window, but the disk still holds the
    // consumed bytes: one hash comparison proves it and earns the fast
    // path.
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");
    age_file(dep);

    FileTable pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      /*build_at=*/1);
    ASSERT_EQ(stamp_of(pool, snap.deps[0]), 0);

    ASSERT_FALSE(changed(pool, snap));
    ASSERT_EQ(stamp_of(pool, snap.deps[0]), file_mtime_ns(dep));
}

TEST_CASE(MissingTransitions) {
    TempDir tmp;
    auto dep = tmp.path("ghost.h");

    FileTable pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, 0}
    },
                                      generous_build_at());
    ASSERT_TRUE(snap.deps[0].missing);

    // Still missing: unchanged.
    ASSERT_FALSE(changed(pool, snap));

    // Appearing is a change.
    tmp.touch("ghost.h", "int f();\n");
    ASSERT_TRUE(changed(pool, snap));
}

TEST_CASE(RemovedAfterBuild) {
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");

    FileTable pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());

    fs::remove(dep);
    ASSERT_TRUE(changed(pool, snap));
}

TEST_CASE(ForceRevalidateGoesByHash) {
    TempDir tmp;
    tmp.touch("dep.h", "int old_name();\n");
    auto dep = tmp.path("dep.h");
    age_file(dep);

    FileTable pool;
    pool.read(pool.intern(dep));
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());
    auto recorded_mtime = stamp_of(pool, snap.deps[0]);
    ASSERT_TRUE(recorded_mtime != 0);

    // An edit that restores the recorded stat exactly would pass the fast
    // path; force_revalidate drops it, so the hash still catches the edit.
    tmp.touch("dep.h", "int new_name();\n");  // same length
    set_file_mtime(dep, recorded_mtime);

    snap.force_revalidate(pool);
    ASSERT_TRUE(changed(pool, snap));
}

TEST_CASE(ForceRevalidatePurgesWaveMemo) {
    // A force point inside a wave must not be bypassed by a verdict the
    // wave already memoized.
    TempDir tmp;
    tmp.touch("dep.h", "int old_name();\n");
    auto dep = tmp.path("dep.h");
    age_file(dep);

    FileTable pool;
    pool.read(pool.intern(dep));
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());
    auto recorded_mtime = stamp_of(pool, snap.deps[0]);

    pool.begin_wave();
    ASSERT_FALSE(deps_changed(pool, snap));

    tmp.touch("dep.h", "int new_name();\n");  // same length
    set_file_mtime(dep, recorded_mtime);
    snap.force_revalidate(pool);
    ASSERT_TRUE(deps_changed(pool, snap));
}

};  // TEST_SUITE(DepsSnapshot)

}  // namespace
}  // namespace clice::testing
