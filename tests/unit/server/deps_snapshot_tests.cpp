#include <chrono>

#include "test/temp_dir.h"
#include "test/test.h"
#include "server/state/workspace.h"

#include "llvm/Support/xxhash.h"

namespace clice::testing {
namespace {

std::int64_t now_seconds() {
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

TEST_SUITE(DepsSnapshot) {

TEST_CASE(AdoptsWorkerHash) {
    TempDir dir;
    dir.touch("dep.h", "int value = 1;");
    PathPool pool;

    // The worker consumed different content than the disk holds (the file
    // was written during the compile window): the artifact must look stale.
    // Only the worker-reported hash can reveal this — the master never saw
    // the consumed bytes.
    HashedDep stale_dep{dir.path("dep.h"), llvm::xxh3_64bits("int value = 0;")};
    auto snap = capture_deps_snapshot(pool, stale_dep, now_seconds() - 60);
    ASSERT_TRUE(deps_changed(pool, snap));

    // Consumed content matches the disk: fresh, even though the mtime is
    // newer than build_at (the hash comparison proves it was a mere touch).
    HashedDep fresh_dep{dir.path("dep.h"), llvm::xxh3_64bits("int value = 1;")};
    snap = capture_deps_snapshot(pool, fresh_dep, now_seconds() - 60);
    ASSERT_FALSE(deps_changed(pool, snap));
}

TEST_CASE(MtimeFastLayer) {
    TempDir dir;
    dir.touch("dep.h", "int value = 1;");
    PathPool pool;

    // An mtime at or before build_at short-circuits the hash comparison.
    // This is why build_at must be the compile start time: with a later
    // baseline (result arrival), a save landing mid-compile would pass
    // this layer and its divergence would never be checked.
    HashedDep dep{dir.path("dep.h"), llvm::xxh3_64bits("something else entirely")};
    auto snap = capture_deps_snapshot(pool, dep, now_seconds() + 60);
    ASSERT_FALSE(deps_changed(pool, snap));
}

TEST_CASE(MissingDepFile) {
    TempDir dir;
    PathPool pool;

    // Missing at build time (hash 0) and still missing: unchanged.
    HashedDep never_existed{dir.path("ghost.h"), 0};
    auto snap = capture_deps_snapshot(pool, never_existed, now_seconds() - 60);
    ASSERT_FALSE(deps_changed(pool, snap));

    // Consumed at build time but deleted since: changed.
    HashedDep deleted{dir.path("ghost.h"), llvm::xxh3_64bits("int value = 1;")};
    snap = capture_deps_snapshot(pool, deleted, now_seconds() - 60);
    ASSERT_TRUE(deps_changed(pool, snap));
}

};  // TEST_SUITE(DepsSnapshot)

}  // namespace
}  // namespace clice::testing
