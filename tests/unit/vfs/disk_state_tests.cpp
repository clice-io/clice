#include <chrono>
#include <format>
#include <string>
#include <vector>

#include "test/temp_dir.h"
#include "test/test.h"
#include "support/anomaly.h"
#include "support/filesystem.h"
#include "vfs/file_table.h"
#include "vfs/path.h"

#include "llvm/Support/xxhash.h"

namespace clice::testing {

namespace {

using Clock = vfs::DiskState::Clock;
using Verdict = vfs::DiskState::Verdict;
using namespace std::chrono_literals;

/// A file table whose schedule runs on a clock the test turns, with a
/// package root at `pkg/` and the workspace everywhere else.
struct Fixture {
    TempDir tmp;
    FileTable table;
    vfs::DiskState& disk = table.disk;
    Clock::time_point time = Clock::now();

    Fixture() {
        disk.now = [this] {
            return time;
        };
        tmp.mkdir("pkg");
        disk.add_root(identity("pkg"), vfs::DiskState::package_policy);
    }

    std::string identity(llvm::StringRef relative) const {
        return CanonicalPath(Spelling::absolute(tmp.path(relative))).str();
    }

    /// A file written long enough ago that its reads vouch for its stamp.
    Fid file(llvm::StringRef relative, llvm::StringRef content) {
        tmp.touch(relative, content);
        auto path = tmp.path(relative);
        EXPECT_TRUE(set_file_mtime(path, file_mtime_ns(path) - 10'000'000'000));
        return table.intern(Spelling::absolute(path));
    }

    void rewrite(llvm::StringRef relative, llvm::StringRef content) {
        tmp.touch(relative, content);
    }

    Verdict check(Fid fid, std::uint64_t hash) {
        auto wave = disk.wave();
        return disk.check(fid, hash);
    }

    std::uint64_t hash_of(Fid fid) {
        auto obs = disk.current(fid);
        EXPECT_TRUE(obs.has_value());
        return obs ? obs->hash : 0;
    }

    llvm::SmallVector<Fid> tick() {
        disk.tick(1h);
        return disk.take_changes();
    }
};

TEST_SUITE(DiskState) {

TEST_CASE(WorkspaceAlwaysLooks) {
    Fixture f;
    auto fid = f.file("src/a.h", "int a;\n");
    auto hash = f.hash_of(fid);
    f.rewrite("src/a.h", "int b;\n");
    ASSERT_TRUE(f.check(fid, hash) == Verdict::Stale);
}

TEST_CASE(PackageTrustedUntilDue) {
    Fixture f;
    auto fid = f.file("pkg/a.h", "int a;\n");
    auto hash = f.hash_of(fid);
    f.rewrite("pkg/a.h", "int b;\n");
    ASSERT_TRUE(f.check(fid, hash) == Verdict::Fresh);

    f.time += vfs::DiskState::package_policy.min;
    ASSERT_TRUE(f.check(fid, hash) == Verdict::Stale);
    ASSERT_EQ(f.disk.take_changes(), llvm::SmallVector<Fid>{fid});
}

TEST_CASE(ChecksCounted) {
    Fixture f;
    auto installed = f.file("pkg/a.h", "int a;\n");
    auto local = f.file("src/b.h", "int b;\n");
    f.check(installed, f.hash_of(installed));
    f.check(local, f.hash_of(local));
    ASSERT_EQ(f.disk.checks.trusted, 1u);
    ASSERT_EQ(f.disk.checks.looked, 1u);
}

TEST_CASE(TrustedMissingStays) {
    // A place a build found empty under a package root is not looked at
    // again before it is due.
    Fixture f;
    auto fid = f.table.intern(Spelling::absolute(f.tmp.path("pkg/none.h")));
    f.disk.saw_missing(fid);
    ASSERT_TRUE(f.disk.take_changes().empty());
    f.file("pkg/none.h", "int a;\n");
    {
        auto wave = f.disk.wave();
        ASSERT_FALSE(f.disk.present(fid));
    }
    f.time += vfs::DiskState::package_policy.min;
    auto wave = f.disk.wave();
    ASSERT_TRUE(f.disk.present(fid));
}

TEST_CASE(TrustOnlyConfirms) {
    // A build that read newer bytes than the last look is not told they
    // changed: only a look may say so.
    Fixture f;
    auto fid = f.file("pkg/a.h", "int a;\n");
    f.hash_of(fid);
    f.rewrite("pkg/a.h", "int b;\n");
    ASSERT_TRUE(f.check(fid, llvm::xxh3_64bits("int b;\n")) == Verdict::Fresh);
    ASSERT_EQ(f.disk.take_changes(), llvm::SmallVector<Fid>{fid});

    // Nor that a place it found empty, after an upgrade removed the file
    // there, is still filled.
    auto gone = f.file("pkg/gone.h", "int c;\n");
    f.hash_of(gone);
    fs::remove_all(f.tmp.path("pkg/gone.h"));
    auto wave = f.disk.wave();
    ASSERT_FALSE(f.disk.present(gone));
}

TEST_CASE(OneLookPerWave) {
    Fixture f;
    auto fid = f.file("src/a.h", "int a;\n");
    auto hash = f.hash_of(fid);
    auto wave = f.disk.wave();
    ASSERT_TRUE(f.disk.check(fid, hash) == Verdict::Fresh);
    ASSERT_TRUE(f.disk.check(fid, hash + 1) == Verdict::Stale);
    ASSERT_EQ(f.disk.checks.looked, 1u);
}

TEST_CASE(DeepestRootDecides) {
    // A workspace root registered after a package root inside it leaves
    // the package's files trusted.
    Fixture f;
    f.disk.add_root(CanonicalPath(Spelling::absolute(f.tmp.root)).str(),
                    vfs::DiskState::workspace_policy);
    auto fid = f.file("pkg/a.h", "int a;\n");
    auto hash = f.hash_of(fid);
    f.rewrite("pkg/a.h", "int b;\n");
    ASSERT_TRUE(f.check(fid, hash) == Verdict::Fresh);
}

TEST_CASE(TickLooksWhenDue) {
    Fixture f;
    auto fid = f.file("pkg/a.h", "int a;\n");
    f.hash_of(fid);
    f.rewrite("pkg/a.h", "int b;\n");
    ASSERT_TRUE(f.tick().empty());
    f.time += vfs::DiskState::package_policy.min;
    ASSERT_EQ(f.tick(), llvm::SmallVector<Fid>{fid});
}

TEST_CASE(QuietLooksBackOff) {
    // An unchanged look doubles the interval; a change drops it back.
    Fixture f;
    auto fid = f.file("src/a.h", "int a;\n");
    f.hash_of(fid);
    f.time += 1s;
    ASSERT_TRUE(f.tick().empty());

    f.rewrite("src/a.h", "int b;\n");
    f.time += 1s;
    ASSERT_TRUE(f.tick().empty());
    f.time += 1s;
    ASSERT_EQ(f.tick(), llvm::SmallVector<Fid>{fid});

    f.rewrite("src/a.h", "int c;\n");
    f.time += 1s;
    ASSERT_EQ(f.tick(), llvm::SmallVector<Fid>{fid});
}

TEST_CASE(BackOffStopsAtMax) {
    Fixture f;
    auto fid = f.file("src/a.h", "int a;\n");
    f.hash_of(fid);
    for(int i = 0; i < 10; i += 1) {
        f.time += vfs::DiskState::workspace_policy.max;
        f.tick();
    }
    f.rewrite("src/a.h", "int b;\n");
    f.time += vfs::DiskState::workspace_policy.max;
    ASSERT_EQ(f.tick(), llvm::SmallVector<Fid>{fid});
}

TEST_CASE(LookPostponesTick) {
    // Any look, a check's included, restarts the wait for the next one.
    Fixture f;
    auto fid = f.file("src/a.h", "int a;\n");
    auto hash = f.hash_of(fid);
    f.time += 900ms;
    ASSERT_TRUE(f.check(fid, hash) == Verdict::Fresh);
    f.rewrite("src/a.h", "int b;\n");
    f.time += 200ms;
    ASSERT_TRUE(f.tick().empty());
}

TEST_CASE(ExpiredLooksAgain) {
    Fixture f;
    auto a = f.file("pkg/a.h", "int a;\n");
    auto b = f.file("pkg/sub/b.h", "int b;\n");
    auto hash = f.hash_of(b);
    f.hash_of(a);
    f.rewrite("pkg/a.h", "int c;\n");
    f.rewrite("pkg/sub/b.h", "int d;\n");

    f.disk.expire_under(f.identity("pkg/sub"));
    ASSERT_TRUE(f.check(b, hash) == Verdict::Stale);
    ASSERT_EQ(f.tick(), llvm::SmallVector<Fid>{b});
}

TEST_CASE(LookNowAtAny) {
    // A save looks at the open documents' files at once, due or not.
    Fixture f;
    auto a = f.file("pkg/a.h", "int a;\n");
    f.hash_of(a);
    f.rewrite("pkg/a.h", "int c;\n");
    f.disk.look(llvm::ArrayRef<Fid>{a, a});
    ASSERT_EQ(f.disk.take_changes(), llvm::SmallVector<Fid>{a});
}

TEST_CASE(RootAddedLater) {
    // A root registered after its files were seen governs them from then.
    Fixture f;
    auto fid = f.file("lib/a.h", "int a;\n");
    auto hash = f.hash_of(fid);
    f.disk.add_root(f.identity("lib"), vfs::DiskState::package_policy);
    f.rewrite("lib/a.h", "int b;\n");
    ASSERT_TRUE(f.check(fid, hash) == Verdict::Fresh);
}

TEST_CASE(BudgetLeavesRest) {
    Fixture f;
    std::vector<Fid> fids;
    for(int i = 0; i < 4; i += 1) {
        auto name = std::format("src/{}.h", i);
        fids.push_back(f.file(name, "int a;\n"));
        f.hash_of(fids.back());
        f.rewrite(name, "int b;\n");
    }
    f.time += 1s;
    f.disk.tick(Clock::duration::zero());
    ASSERT_EQ(f.disk.take_changes().size(), 1u);
    f.disk.tick(1h);
    ASSERT_EQ(f.disk.take_changes().size(), 3u);
}

TEST_CASE(ConsumedIsFirstLook) {
    // A build's read of a file nobody looked at stands as the first look:
    // the disk moving on from it is a change.
    Fixture f;
    auto fid = f.file("src/a.h", "int b;\n");
    f.disk.consumed(fid, llvm::xxh3_64bits("int a;\n"));
    f.hash_of(fid);
    ASSERT_EQ(f.disk.take_changes(), llvm::SmallVector<Fid>{fid});

    f.disk.consumed(fid, llvm::xxh3_64bits("int a;\n"));
    ASSERT_EQ(f.disk.seen_hash(fid), llvm::xxh3_64bits("int b;\n"));
}

TEST_CASE(FlagHearsChange) {
    Fixture f;
    f.file(".git/HEAD", "ref: refs/heads/main\n");
    int heard = 0;
    auto flag = f.disk.watch(f.tmp.path(".git/HEAD"), [&] { heard += 1; });
    f.tick();
    ASSERT_EQ(heard, 0);

    f.rewrite(".git/HEAD", "ref: refs/heads/other\n");
    f.tick();
    ASSERT_EQ(heard, 1);
    ASSERT_EQ(flag->hash, llvm::xxh3_64bits("ref: refs/heads/other\n"));
    f.tick();
    ASSERT_EQ(heard, 1);

    flag.reset();
    f.rewrite(".git/HEAD", "ref: refs/heads/main\n");
    f.tick();
    ASSERT_EQ(heard, 1);
}

TEST_CASE(EnvironmentInstallMakesDue) {
    // An install rewrites the environment's history: everything installed
    // in it falls due.
    Fixture f;
    f.file("env/conda-meta/history", "==> 1 <==\n");
    auto fid = f.file("env/include/a.h", "int a;\n");
    f.disk.add_package(f.identity("env/include"));
    auto hash = f.hash_of(fid);
    f.rewrite("env/include/a.h", "int b;\n");
    ASSERT_TRUE(f.check(fid, hash) == Verdict::Fresh);

    // pixi rewrites the history with the same line every time.
    f.rewrite("env/conda-meta/history", "==> 1 <==\n");
    ASSERT_EQ(f.tick(), llvm::SmallVector<Fid>{fid});
}

TEST_CASE(ShadowReportsStaleTrust) {
    logging::reset_anomaly_for_testing();
    std::vector<logging::AnomalyId> trapped;
    logging::set_anomaly_trap_for_testing([&](logging::AnomalyId id) { trapped.push_back(id); });

    Fixture f;
    f.disk.shadow = true;
    auto fid = f.file("pkg/a.h", "int a;\n");
    auto hash = f.hash_of(fid);
    ASSERT_TRUE(f.check(fid, hash) == Verdict::Fresh);
    ASSERT_TRUE(trapped.empty());

    f.rewrite("pkg/a.h", "int b;\n");
    ASSERT_TRUE(f.check(fid, hash) == Verdict::Fresh);
    ASSERT_EQ(trapped.size(), 1u);
    ASSERT_EQ(trapped[0], logging::AnomalyId::StaleTrust);

    logging::reset_anomaly_for_testing();
}

};  // TEST_SUITE(DiskState)

}  // namespace

}  // namespace clice::testing
