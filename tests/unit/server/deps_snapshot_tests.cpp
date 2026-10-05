#include "test/temp_dir.h"
#include "test/test.h"
#include "project/project.h"
#include "vfs/file_system.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {
namespace {

/// A build_at (milliseconds since epoch, like the worker's `unit.build_at()`)
/// far enough in the future that every existing file clears the mtime guard.
std::int64_t generous_build_at() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count() +
           10'000;
}

/// The consumed hash a worker would report for the file's current bytes
/// (0 = unreadable, which no test here expects).
std::uint64_t consumed_hash(llvm::StringRef path) {
    auto text = vfs::read(path);
    return text ? llvm::xxh3_64bits((*text)->getBuffer()) : 0;
}

/// Rewind a file's mtime out of the mtime-granularity guard window, the
/// way real project files predate a server start. A freshly touched file
/// is deliberately untrusted (see vfs::read_observed), so tests exercising
/// the stat fast path must age their files first.
void age_file(llvm::StringRef path) {
    ZEXPECT(set_file_mtime(path, file_mtime_ns(path) - 10'000'000'000));
}

/// Whether the file table answers the file's current stat without a read.
bool vouched(FileTable& pool, llvm::StringRef path) {
    auto status = vfs::status(path);
    return status && pool.cached_hash(pool.intern(Spelling::absolute(path)), status->stamp);
}

ZEST_SUITE(DepsSnapshot) {

ZEST_CASE(FreshWhenUntouched) {
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
    ZASSERT(snap.size() == 1u);
    ZASSERT(!deps_changed(pool, snap));

    // The check's read left the file's pair behind: the next check is a
    // stat.
    ZASSERT(vouched(pool, dep));
}

ZEST_CASE(SnapshotsShareOneVersion) {
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");
    age_file(dep);

    FileTable pool;
    auto build_at = generous_build_at();
    auto first = capture_deps_snapshot(pool,
                                       {
                                           DepFile{dep, consumed_hash(dep)}
    },
                                       build_at);
    auto second = capture_deps_snapshot(pool,
                                        {
                                            DepFile{dep, consumed_hash(dep)}
    },
                                        build_at);
    ZASSERT(pool.versions.size() == 1u);

    // One check's read serves the other snapshot.
    ZASSERT(!deps_changed(pool, first));
    ZASSERT(vouched(pool, dep));
    ZASSERT(!deps_changed(pool, second));
}

ZEST_CASE(ImmediateEditDetected) {
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
    ZASSERT(deps_changed(pool, snap));
}

ZEST_CASE(EveryChangeFound) {
    // One check finds every changed dependency, so they cascade together.
    TempDir tmp;
    tmp.touch("a.h", "int a();\n");
    tmp.touch("b.h", "int b();\n");
    auto a = tmp.path("a.h");
    auto b = tmp.path("b.h");
    age_file(a);
    age_file(b);

    FileTable pool;
    pool.read(pool.intern(Spelling::absolute(a)));
    pool.read(pool.intern(Spelling::absolute(b)));
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{a, consumed_hash(a)},
                                          DepFile{b, consumed_hash(b)},
    },
                                      generous_build_at());
    tmp.touch("a.h", "int a2();\n");
    tmp.touch("b.h", "int b2();\n");
    ZASSERT(deps_changed(pool, snap));
    ZASSERT(pool.take_changes().size() == 2u);
}

ZEST_CASE(BackdatedEditDetected) {
    // The F04 shape: the edit lands with an mtime that does not move
    // forward (rsync -t, git-restore-mtime). Equality comparison sends it
    // to the hash layer regardless of the timestamp's direction.
    TempDir tmp;
    tmp.touch("dep.h", "int old_name();\n");
    auto dep = tmp.path("dep.h");
    age_file(dep);

    FileTable pool;
    pool.read(pool.intern(Spelling::absolute(dep)));
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());
    auto recorded_mtime = file_mtime_ns(dep);
    ZASSERT(vouched(pool, dep));

    tmp.touch("dep.h", "int new_name();\n");  // same length
    ZASSERT(set_file_mtime(dep, recorded_mtime - 5'000'000'000));
    ZASSERT(deps_changed(pool, snap));
}

ZEST_CASE(TouchRepairsFastPath) {
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");
    age_file(dep);

    FileTable pool;
    pool.read(pool.intern(Spelling::absolute(dep)));
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());

    // Rewrite identical bytes: the stat moves, the content does not.
    auto before = file_mtime_ns(dep);
    tmp.touch("dep.h", "int f();\n");
    ZASSERT(set_file_mtime(dep, before + 5'000'000'000));
    ZASSERT(!vouched(pool, dep));
    ZASSERT(!deps_changed(pool, snap));

    // The check's read moved the pair to the new stat.
    ZASSERT(vouched(pool, dep));
}

ZEST_CASE(PoisonedCaptureDetected) {
    // The F01 shape: the dependency changed between the build reading it
    // and the snapshot being captured. The consumed hash describes v1, the
    // disk holds v2, and the capture-time stat must not bless v2.
    TempDir tmp;
    tmp.touch("dep.h", "int v1();\n");
    auto dep = tmp.path("dep.h");
    auto consumed = consumed_hash(dep);

    tmp.touch("dep.h", "int v2();\n");
    FileTable pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed}
    },
                                      /*build_at=*/1);
    ZASSERT(deps_changed(pool, snap));
}

ZEST_CASE(StalePairRereads) {
    // The pair describes bytes the scan read; after an edit the live stat
    // no longer matches it, so the check reads instead of answering with
    // the old hash.
    TempDir tmp;
    tmp.touch("dep.h", "int v1();\n");
    auto dep = tmp.path("dep.h");

    FileTable pool;
    age_file(dep);
    pool.read(pool.intern(Spelling::absolute(dep)));

    tmp.touch("dep.h", "int v2();\n");
    age_file(dep);
    // Same-tick touches can age to a stat identical to the pair's on
    // coarse-timestamp filesystems, and an equal-stat same-size rewrite
    // is the accepted mtime residual — the premise here is a stat that
    // does differ, so force it apart.
    ZASSERT(set_file_mtime(dep, file_mtime_ns(dep) - 5'000'000'000));
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());
    ZASSERT(!vouched(pool, dep));
    ZASSERT(!deps_changed(pool, snap));
    ZASSERT(vouched(pool, dep));
}

ZEST_CASE(MissingTransitions) {
    TempDir tmp;
    auto dep = tmp.path("ghost.h");

    FileTable pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, 0}
    },
                                      generous_build_at());
    ZASSERT(snap[0].missing);

    // Still missing: unchanged.
    ZASSERT(!deps_changed(pool, snap));

    // Appearing is a change, and the file table saw it.
    tmp.touch("ghost.h", "int f();\n");
    ZASSERT(deps_changed(pool, snap));
    ZASSERT(!pool.seen_missing(pool.intern(Spelling::absolute(dep))));
}

ZEST_CASE(AbsentPlaceFilled) {
    // A place a failed lookup looked holds a file by the capture: the build
    // never saw it, so the artifact is stale however old the file is.
    TempDir tmp;
    tmp.touch("gen.h", "int make();\n");
    auto place = tmp.path("gen.h");
    age_file(place);

    FileTable pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{.path = place, .absent = true}
    },
                                      generous_build_at());
    ZASSERT(snap[0].missing);
    ZASSERT(deps_changed(pool, snap));
}

ZEST_CASE(RemovedAfterBuild) {
    TempDir tmp;
    tmp.touch("dep.h", "int f();\n");
    auto dep = tmp.path("dep.h");

    FileTable pool;
    auto snap = capture_deps_snapshot(pool,
                                      {
                                          DepFile{dep, consumed_hash(dep)}
    },
                                      generous_build_at());

    vfs::remove(dep);
    ZASSERT(deps_changed(pool, snap));
}

};  // ZEST_SUITE(DepsSnapshot)

}  // namespace
}  // namespace clice::testing
