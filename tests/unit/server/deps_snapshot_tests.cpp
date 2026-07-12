#include "test/temp_dir.h"
#include "test/test.h"
#include "server/state/workspace.h"

#include "llvm/Support/FileSystem.h"

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

TEST_SUITE(DepsSnapshot) {

TEST_CASE(FreshWhenUntouched) {
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");

    PathPool pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, hash_file(dep)}
    },
                                      generous_build_at());
    ASSERT_EQ(snap.deps.size(), 1u);
    ASSERT_TRUE(snap.deps[0].mtime_ns != 0);
    ASSERT_FALSE(deps_changed(pool, snap));
}

TEST_CASE(ImmediateEditDetected) {
    // The F44 shape: the dependency is saved right after the artifact's
    // freshness was captured — no watermark may bless it.
    TempDir tmp;
    tmp.touch("dep.h", "int value();\n");
    auto dep = tmp.path("dep.h");

    PathPool pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, hash_file(dep)}
    },
                                      generous_build_at());
    tmp.touch("dep.h", "int renamed();\n");
    ASSERT_TRUE(deps_changed(pool, snap));
}

TEST_CASE(BackdatedEditDetected) {
    // The F04 shape: the edit lands with an mtime that does not move
    // forward (rsync -t, git-restore-mtime). Equality comparison sends it
    // to the hash layer regardless of the timestamp's direction.
    TempDir tmp;
    tmp.touch("dep.h", "int old_name();\n");
    auto dep = tmp.path("dep.h");

    PathPool pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, hash_file(dep)}
    },
                                      generous_build_at());

    tmp.touch("dep.h", "int new_name();\n");  // same length
    set_file_mtime(dep, snap.deps[0].mtime_ns - 5'000'000'000);
    ASSERT_TRUE(deps_changed(pool, snap));
}

TEST_CASE(TouchRepairsFastPath) {
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");

    PathPool pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, hash_file(dep)}
    },
                                      generous_build_at());

    // Rewrite identical bytes: the stat moves, the content does not.
    tmp.touch("dep.h", "int f();\n");
    set_file_mtime(dep, snap.deps[0].mtime_ns + 5'000'000'000);
    ASSERT_FALSE(deps_changed(pool, snap));

    // The passing hash comparison repaired the fast path in place.
    ASSERT_EQ(snap.deps[0].mtime_ns, file_mtime_ns(dep));
}

TEST_CASE(PoisonedCaptureDetected) {
    // The F01 shape: the dependency changed between the build reading it
    // and the snapshot being captured. The consumed hash describes v1, the
    // disk holds v2, and the capture-time stat must not bless v2.
    TempDir tmp;
    tmp.touch("dep.h", "int v1();\n");
    auto dep = tmp.path("dep.h");
    auto consumed = hash_file(dep);

    tmp.touch("dep.h", "int v2();\n");
    // build_at in the past: the file's mtime falls inside "modified during
    // or after the build", so no fast path is recorded.
    PathPool pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed}
    },
                                      /*build_at=*/1);
    ASSERT_EQ(snap.deps[0].mtime_ns, 0);
    ASSERT_TRUE(deps_changed(pool, snap));
}

TEST_CASE(NoBaselineConverges) {
    // Same capture shape as above, but the disk still holds the consumed
    // bytes: one hash comparison proves it and re-earns the fast path.
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");

    PathPool pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, hash_file(dep)}
    },
                                      /*build_at=*/1);
    ASSERT_EQ(snap.deps[0].mtime_ns, 0);

    ASSERT_FALSE(deps_changed(pool, snap));
    ASSERT_EQ(snap.deps[0].mtime_ns, file_mtime_ns(dep));
}

TEST_CASE(MissingTransitions) {
    TempDir tmp;
    auto dep = tmp.path("ghost.h");

    PathPool pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, 0}
    },
                                      generous_build_at());
    ASSERT_TRUE(snap.deps[0].missing);

    // Still missing: unchanged.
    ASSERT_FALSE(deps_changed(pool, snap));

    // Appearing is a change.
    tmp.touch("ghost.h", "int f();\n");
    ASSERT_TRUE(deps_changed(pool, snap));
}

TEST_CASE(RemovedAfterBuild) {
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");

    PathPool pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, hash_file(dep)}
    },
                                      generous_build_at());

    fs::remove(dep);
    ASSERT_TRUE(deps_changed(pool, snap));
}

TEST_CASE(ForceRevalidateGoesByHash) {
    TempDir tmp;
    tmp.touch("dep.h", "int old_name();\n");
    auto dep = tmp.path("dep.h");

    PathPool pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, hash_file(dep)}
    },
                                      generous_build_at());
    auto recorded_mtime = snap.deps[0].mtime_ns;

    // An edit that restores the recorded stat exactly would pass the fast
    // path; force_revalidate drops it, so the hash still catches the edit.
    tmp.touch("dep.h", "int new_name();\n");  // same length
    set_file_mtime(dep, recorded_mtime);

    snap.force_revalidate();
    ASSERT_TRUE(deps_changed(pool, snap));
}

};  // TEST_SUITE(DepsSnapshot)

}  // namespace
}  // namespace clice::testing
