module;

#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :command.database;
import :settings.settings;
import :unittests.server.served_project_fixture;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.disk_state;
import :vfs.ids;
import :vfs.path;

namespace clice::testing {
namespace {

/// A project served from `root`, its databases loaded by the test and then
/// watched (watch_databases).
using Fixture = ServedProjectFixture;

/// A master tick as the databases' watch sees it: the file table looks at
/// the flags, then every database is ticked against what the looks found.
CDBDiff tick(Fixture& fixture, bool force = false) {
    fixture.server.files.disk.look_flags();
    return fixture.database_delta(force);
}

/// What one workspace tick of the test hook saw: a look at every file, then
/// the build's default sources refreshed.
struct WorkspaceTick {
    /// A file created under a default-command rule joined the build.
    bool joined = false;

    /// The files the looks found changed on disk.
    llvm::SmallVector<Fid> changes;

    bool empty() const {
        return !joined && changes.empty();
    }
};

WorkspaceTick workspace_tick(Fixture& fixture) {
    auto& disk = fixture.server.files.disk;
    disk.look_all();
    auto [joined] = kota::run(fixture.served->poll_sources());
    return {.joined = *joined, .changes = disk.take_changes()};
}

ZEST_SUITE(Databases) {

ZEST_CASE(CDBTickDebounces) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("lib.cpp", R"(int lib() { return 1; })");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    fixture.watch_databases();

    // Rewrite with one more entry: the first tick only records the pending
    // content, the second sees it stable and reloads.
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}},
                  {tmp.root, tmp.path("lib.cpp"),  {}}
    }));
    ZASSERT(tick(fixture).empty());

    auto delta = tick(fixture);
    auto lib_id = fixture.intern(tmp.path("lib.cpp"));
    ZASSERT(delta.added == llvm::SmallVector<Fid>{lib_id});
    ZASSERT(delta.removed.empty());

    // Stable: further ticks are quiet.
    ZASSERT(tick(fixture).empty());
}

ZEST_CASE(CDBTickForceImmediate) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    fixture.watch_databases();

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DFOO"}}
    }));
    auto delta = tick(fixture, /*force=*/true);
    auto main_id = fixture.intern(tmp.path("main.cpp"));
    ZASSERT(delta.changed == llvm::SmallVector<Fid>{main_id});
}

ZEST_CASE(CDBTickDiscoversLate) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    // No compile_commands.json when the watch starts.
    Fixture fixture(tmp.root);
    fixture.watch_databases();
    ZASSERT(tick(fixture, /*force=*/true).empty());

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    auto delta = tick(fixture, /*force=*/true);
    auto main_id = fixture.intern(tmp.path("main.cpp"));
    ZASSERT(delta.added == llvm::SmallVector<Fid>{main_id});
}

ZEST_CASE(CDBTickWatchesSubdirectory) {
    /// A database generated into an existing build directory is found
    /// where the tick watches for it, and stabilizes like a rewrite.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.mkdir("build");
    Fixture fixture(tmp.root);
    fixture.watch_databases();
    ZASSERT(tick(fixture).empty());

    tmp.touch("build/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    ZASSERT(tick(fixture).empty());
    auto delta = tick(fixture);
    auto main_id = fixture.intern(tmp.path("main.cpp"));
    ZASSERT(delta.added == llvm::SmallVector<Fid>{main_id});
}

ZEST_CASE(CDBTickNewSubdirectory) {
    /// A build directory created after startup is listed once the root
    /// directory moves.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    ZASSERT(set_file_mtime(tmp.root, file_mtime_ns(tmp.root) - 10'000'000'000));
    Fixture fixture(tmp.root);
    fixture.watch_databases();
    ZASSERT(tick(fixture).empty());

    tmp.touch("out/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    ZASSERT(tick(fixture).empty());
    ZASSERT(!tick(fixture).empty());
}

#ifndef _WIN32
ZEST_CASE(CDBTickDanglingSymlink) {
    /// A build directory symlinked to a target created later is watched
    /// from the start.
    TempDir tmp;
    TempDir elsewhere;
    tmp.touch("main.cpp", R"(int main() {})");
    ZASSERT(::symlink(elsewhere.path("target").c_str(), tmp.path("build").c_str()) == 0);
    ZASSERT(set_file_mtime(tmp.root, file_mtime_ns(tmp.root) - 10'000'000'000));
    Fixture fixture(tmp.root);
    fixture.watch_databases();
    ZASSERT(tick(fixture).empty());

    elsewhere.touch("target/compile_commands.json",
                    build_cdb_json({
                        {tmp.root, tmp.path("main.cpp"), {}}
    }));
    ZASSERT(tick(fixture).empty());
    ZASSERT(!tick(fixture).empty());
}
#endif

ZEST_CASE(CDBTickAboveOpenFile) {
    /// A database generated above an open file still without a command is
    /// found where the tick watches for it.
    TempDir tmp;
    tmp.touch("a/b/main.cpp", R"(int main() {})");
    Fixture fixture(tmp.root);
    fixture.watch_databases();
    auto main_id = fixture.open(tmp.path("a/b/main.cpp"), "")->path_id;
    ZASSERT(tick(fixture).empty());

    tmp.touch("a/b/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/b/main.cpp"), {}}
    }));
    ZASSERT(tick(fixture).empty());
    auto delta = tick(fixture);
    ZASSERT(delta.added == llvm::SmallVector<Fid>{main_id});
}

ZEST_CASE(CDBTickDeleteRecreate) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    fixture.watch_databases();

    // Deletion (mid-regeneration): keep serving the loaded entries.
    fs::remove_all(tmp.path("compile_commands.json"));
    ZASSERT(tick(fixture, /*force=*/true).empty());

    // The rewrite arrives as a normal change once the file is back.
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DFOO"}}
    }));
    auto delta = tick(fixture, /*force=*/true);
    auto main_id = fixture.intern(tmp.path("main.cpp"));
    ZASSERT(delta.changed == llvm::SmallVector<Fid>{main_id});
}

ZEST_CASE(CDBTickRetriesFailedLoad) {
    /// A declared database unreadable at startup loads on a later tick even
    /// when its stat is unchanged by then.
    TempDir tmp;
    tmp.touch("compile_commands.json", "[ ");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    auto id = project.cdb.add_source(Spelling::absolute(tmp.path("compile_commands.json")));
    ZASSERT(!project.cdb.load_source(id).has_value());
    auto before = file_mtime_ns(tmp.path("compile_commands.json"));
    fixture.watch_databases();

    tmp.touch("compile_commands.json", "[]");
    ZASSERT(set_file_mtime(tmp.path("compile_commands.json"), before));

    ZASSERT(tick(fixture).empty());
    ZASSERT(tick(fixture).empty());
    ZEXPECT(project.cdb.loaded(id));
}

ZEST_CASE(CDBTickRelocates) {
    /// The discovered database is deleted and one appears elsewhere: the
    /// old entries keep serving, the new database loads through the usual
    /// path, and the files both list change command — the present
    /// database ranks first — until the original returns.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("only.cpp", R"(int only() {})");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    auto original = build_cdb_json({
        {tmp.root, tmp.path("main.cpp"), {}},
        {tmp.root, tmp.path("only.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, original);
    fixture.watch_databases();
    auto main_id = fixture.intern(tmp.path("main.cpp"));
    auto only_id = fixture.intern(tmp.path("only.cpp"));
    auto root = *project.cdb.find_source(Spelling::absolute(tmp.path("compile_commands.json")));

    fs::remove_all(tmp.path("compile_commands.json"));
    ZASSERT(tick(fixture, /*force=*/true).empty());
    ZEXPECT(!project.cdb.present(root));
    ZEXPECT(!project.cdb.candidate_entries(only_id).empty());

    tmp.touch("build/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DMOVED"}}
    }));
    auto delta = tick(fixture, /*force=*/true);
    auto build =
        *project.cdb.find_source(Spelling::absolute(tmp.path("build/compile_commands.json")));
    ZASSERT(delta.changed == llvm::SmallVector<Fid>{main_id});
    ZEXPECT(project.build.entries(main_id).front().source == build);
    ZEXPECT(project.build.entries(only_id).front().source == root);

    tmp.touch("compile_commands.json", original);
    delta = tick(fixture, /*force=*/true);
    ZASSERT(delta.changed == llvm::SmallVector<Fid>{main_id});
    ZEXPECT(project.build.entries(main_id).front().source == root);
    ZEXPECT(project.build.entries(main_id).size() == 2u);
}

ZEST_CASE(CDBDeletedBeforeWatch) {
    /// A database loaded, then deleted before the watch starts: the load's
    /// read is the baseline, so the deletion stabilizes like any other.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    auto id = *project.cdb.find_source(Spelling::absolute(tmp.path("compile_commands.json")));
    ZASSERT(project.cdb.present(id));
    fs::remove_all(tmp.path("compile_commands.json"));
    fixture.watch_databases();
    ZEXPECT(tick(fixture).empty());
    ZEXPECT(tick(fixture).empty());
    ZEXPECT(!project.cdb.present(id));
}

ZEST_CASE(ResponseRewriteBeforeWatch) {
    /// A response file rewritten between the startup load and the watch:
    /// the load's own read is the baseline, so the flags in memory catch
    /// up.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("flags.rsp", "-DONE\n");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"@flags.rsp"}}
    }));
    tmp.touch("flags.rsp", "-DTWO\n");
    fixture.watch_databases();
    ZEXPECT(tick(fixture).empty());
    auto delta = tick(fixture);
    auto main = fixture.intern(tmp.path("main.cpp"));
    ZEXPECT(delta.changed == llvm::SmallVector<Fid>{main});
    ZEXPECT(tick(fixture).empty());
}

ZEST_CASE(ResponseAddedByReload) {
    /// A response file a reload starts reading is watched from then on.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("flags.rsp", "-DONE\n");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    fixture.watch_databases();
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"@flags.rsp"}}
    }));
    ZASSERT(!tick(fixture, /*force=*/true).empty());

    tmp.touch("flags.rsp", "-DTWO\n");
    ZEXPECT(tick(fixture).empty());
    auto delta = tick(fixture);
    auto main = fixture.intern(tmp.path("main.cpp"));
    ZEXPECT(delta.changed == llvm::SmallVector<Fid>{main});
}

ZEST_CASE(CDBTickRenameOver) {
    /// A same-size rewrite renamed over the database within one mtime
    /// tick is a new file: an ordinary tick sees it.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DAAA"}}
    }));
    fixture.watch_databases();
    auto before = file_mtime_ns(tmp.path("compile_commands.json"));

    tmp.touch("replacement.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DBBB"}}
    }));
    ZASSERT(set_file_mtime(tmp.path("replacement.json"), before));
    ZASSERT(!fs::rename(tmp.path("replacement.json"), tmp.path("compile_commands.json")));

    ZASSERT(tick(fixture).empty());
    auto delta = tick(fixture);
    auto main_id = fixture.intern(tmp.path("main.cpp"));
    ZASSERT(delta.changed == llvm::SmallVector<Fid>{main_id});
}

ZEST_CASE(CDBDiscoverRetriesRegistered) {
    /// A database registered but never loaded — remembered from an earlier
    /// session, absent at startup — loads when a file under it is opened
    /// after it appeared.
    TempDir tmp;
    tmp.touch("a/main.cpp", R"(int main() {})");
    Fixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto& project = served.project;
    project.settings.finalize(project.root);
    project.build.reset_active("");
    auto id = project.cdb.add_source(Spelling::absolute(tmp.path("a/compile_commands.json")));
    fixture.watch_databases();
    auto main = fixture.intern(tmp.path("a/main.cpp"));
    // Every change of the build the project follows moves the commands'
    // epoch on.
    auto epoch = project.commands_epoch;
    served.discover_around(main);
    ZEXPECT(project.commands_epoch == epoch);

    tmp.touch("a/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/main.cpp"), {}}
    }));
    served.discover_around(main);
    ZEXPECT(project.commands_epoch == epoch + 1);
    ZEXPECT(!project.build.entries(main).empty());
    ZEXPECT(served.sched.pump.save_debt() == llvm::SmallVector<Fid>{main});
    ZEXPECT(project.cdb.loaded(id));
    served.discover_around(main);
    ZEXPECT(project.commands_epoch == epoch + 1);
}

#ifndef _WIN32
ZEST_CASE(CDBTickFollowsRetarget) {
    /// A database reached through a symlink: pointing the link at another
    /// file is a change, though neither file was written.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("debug.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DDEBUG"}}
    }));
    tmp.touch("release.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DRELEASE"}}
    }));
    auto database = tmp.path("compile_commands.json");
    ZASSERT(::symlink(tmp.path("debug.json").c_str(), database.c_str()) == 0);
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    auto id = project.cdb.add_source(Spelling::absolute(database));
    ZASSERT(project.cdb.load_source(id));
    fixture.watch_databases();

    fs::remove(database);
    ZASSERT(::symlink(tmp.path("release.json").c_str(), database.c_str()) == 0);
    ZASSERT(tick(fixture).empty());
    auto delta = tick(fixture);
    auto main_id = fixture.intern(tmp.path("main.cpp"));
    ZASSERT(delta.changed == llvm::SmallVector<Fid>{main_id});
}
#endif

ZEST_CASE(CDBTickDiscoversAround) {
    /// Opening a file registers the databases above it up to the root, at
    /// once; a file with a command, or outside the workspace, registers
    /// nothing, and a database above a file still without a command is
    /// found by a later tick.
    TempDir tmp;
    tmp.touch("a/b/main.cpp", R"(int main() {})");
    tmp.touch("a/other.cpp", R"(int other() {})");
    Fixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto& project = served.project;
    project.settings.finalize(project.root);
    project.build.reset_active("");
    tmp.touch("a/b/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/b/main.cpp"), {}}
    }));
    fixture.watch_databases();
    auto main_id = fixture.intern(tmp.path("a/b/main.cpp"));
    auto other_id = fixture.intern(tmp.path("a/other.cpp"));

    // Every change of the build the project follows moves the commands'
    // epoch on.
    auto epoch = project.commands_epoch;
    served.discover_around(main_id);
    ZASSERT(project.commands_epoch == epoch + 1);
    ZASSERT(!project.build.entries(main_id).empty());
    ZASSERT(served.sched.pump.save_debt() == llvm::SmallVector<Fid>{main_id});
    served.discover_around(main_id);
    ZEXPECT(project.commands_epoch == epoch + 1);
    served.discover_around(other_id);
    ZEXPECT(project.commands_epoch == epoch + 1);
    auto outside = fixture.intern(path::join(path::parent(tmp.root), "x.cpp"));
    served.discover_around(outside);
    ZEXPECT(project.commands_epoch == epoch + 1);

    fixture.open(tmp.path("a/other.cpp"), "");
    tmp.touch("a/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/other.cpp"), {}}
    }));
    auto delta = tick(fixture, /*force=*/true);
    ZASSERT(delta.added == llvm::SmallVector<Fid>{other_id});
}

ZEST_CASE(CDBTickPhantomReplacement) {
    /// A replacement that does not parse appears while the original is
    /// gone, the original comes back, then the replacement is repaired:
    /// nothing ever left, and the repaired database only adds what the
    /// original lacks.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("other.cpp", R"(int other() {})");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    auto original = build_cdb_json({
        {tmp.root, tmp.path("main.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, original);
    fixture.watch_databases();
    auto main_id = fixture.intern(tmp.path("main.cpp"));
    auto other_id = fixture.intern(tmp.path("other.cpp"));
    auto root = *project.cdb.find_source(Spelling::absolute(tmp.path("compile_commands.json")));

    fs::remove_all(tmp.path("compile_commands.json"));
    ZASSERT(tick(fixture, /*force=*/true).empty());
    tmp.touch("build/compile_commands.json", "not a database");
    ZASSERT(tick(fixture, /*force=*/true).empty());

    tmp.touch("compile_commands.json", original);
    ZASSERT(tick(fixture, /*force=*/true).empty());
    ZEXPECT(project.cdb.present(root));

    tmp.touch("build/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("other.cpp"), {}}
    }));
    auto delta = tick(fixture, /*force=*/true);
    ZASSERT(delta.added == llvm::SmallVector<Fid>{other_id});
    ZEXPECT(project.build.entries(main_id).size() == 1u);
    ZEXPECT(!project.cdb.candidate_entries(other_id).empty());
}

ZEST_CASE(CDBTickCoalescesSources) {
    /// Two databases stabilizing in one tick make one delta.
    TempDir tmp;
    tmp.touch("a/main.cpp", R"(int main() {})");
    tmp.touch("b/other.cpp", R"(int other() {})");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    project.settings.finalize(project.root);
    project.build.reset_active("");
    auto a = project.cdb.add_source(Spelling::absolute(tmp.path("a/compile_commands.json")));
    auto b = project.cdb.add_source(Spelling::absolute(tmp.path("b/compile_commands.json")));
    fixture.watch_databases();
    tmp.touch("a/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/main.cpp"), {}}
    }));
    tmp.touch("b/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("b/other.cpp"), {}}
    }));
    ZEXPECT(tick(fixture).empty());
    auto delta = tick(fixture);
    ZEXPECT(delta.added.size() == 2u);
    ZEXPECT(project.cdb.loaded(a));
    ZEXPECT(project.cdb.loaded(b));
}

ZEST_CASE(CDBRewriteBeforeWatch) {
    /// A rewrite arriving between the load and the watch is a change: the
    /// baseline is the load's own read, not a stat taken afterwards.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DAAA"}}
    }));
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DLONGER"}}
    }));
    fixture.watch_databases();

    ZASSERT(tick(fixture).empty());
    auto delta = tick(fixture);
    auto main_id = fixture.intern(tmp.path("main.cpp"));
    ZASSERT(delta.changed == llvm::SmallVector<Fid>{main_id});
    ZASSERT(tick(fixture).empty());
}

ZEST_CASE(CDBSameStampRewrite) {
    /// A same-size rewrite in place within the mtime granularity of the
    /// load leaves the stat untouched: the stat of a fresh load cannot
    /// confirm the bytes, so the ticks compare the content.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DAAA"}}
    }));
    // An mtime not yet safely in the past, however long the load takes.
    auto database = tmp.path("compile_commands.json");
    auto mtime = file_mtime_ns(database) + 3'600'000'000'000;
    ZASSERT(set_file_mtime(database, mtime));
    ZASSERT(project.cdb.reload_and_diff(SourceID(0)));
    fixture.watch_databases();
    ZASSERT(tick(fixture).empty());

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DBBB"}}
    }));
    ZASSERT(set_file_mtime(database, mtime));

    ZASSERT(tick(fixture).empty());
    auto delta = tick(fixture);
    auto main_id = fixture.intern(tmp.path("main.cpp"));
    ZASSERT(delta.changed == llvm::SmallVector<Fid>{main_id});
    ZASSERT(tick(fixture).empty());
}

ZEST_CASE(CDBForgedStampSeen) {
    /// A rewrite that puts back a size and mtime safely in the past still
    /// moves the change time: the watch reads it and reloads.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DAAA"}}
    }));
    auto database = tmp.path("compile_commands.json");
    auto mtime = file_mtime_ns(database) - 3'600'000'000'000;
    ZASSERT(set_file_mtime(database, mtime));
    ZASSERT(project.cdb.reload_and_diff(SourceID(0)));
    fixture.watch_databases();
    ZASSERT(tick(fixture).empty());
    ZASSERT(tick(fixture).empty());

    // Time is the feature: past the coarse clock inode times are taken from.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DBBB"}}
    }));
    ZASSERT(set_file_mtime(database, mtime));
    ZASSERT(tick(fixture).empty());
    ZASSERT(!tick(fixture).empty());
}

ZEST_CASE(WorkspaceTickStateMachine) {
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");
    Fixture fixture(tmp.root);
    auto& disk = fixture.server.files.disk;
    auto header = fixture.intern(tmp.path("header.h"));
    fixture.watch_databases();

    // A first look is no change.
    disk.current(header);
    ZASSERT(workspace_tick(fixture).empty());

    // Content change is confirmed by hash and reported once. The new
    // content has a different LENGTH on purpose: back-to-back writes can
    // fall within one mtime tick (observed on Windows CI), and only the
    // size change keeps the stamp fast path deterministic.
    tmp.touch("header.h", R"(int x = 2222;)");
    auto changed = workspace_tick(fixture);
    ZASSERT(!changed.joined);
    ZASSERT(changed.changes == llvm::SmallVector<Fid>{header});
    ZASSERT(!disk.seen_missing(header));

    // Touch: mtime may bump, identical bytes — silent either way.
    tmp.touch("header.h", R"(int x = 2222;)");
    ZASSERT(workspace_tick(fixture).empty());

    // Removal reported once, then quiet while missing.
    fs::remove_all(tmp.path("header.h"));
    auto removed = workspace_tick(fixture);
    ZASSERT(!removed.joined);
    ZASSERT(removed.changes == llvm::SmallVector<Fid>{header});
    ZASSERT(disk.seen_missing(header));
    ZASSERT(workspace_tick(fixture).empty());

    // Reappearance counts as a disk change.
    tmp.touch("header.h", R"(int x = 3;)");
    auto reborn = workspace_tick(fixture);
    ZASSERT(!reborn.joined);
    ZASSERT(reborn.changes.size() == 1u);
    ZASSERT(!disk.seen_missing(reborn.changes.front()));
}

ZEST_CASE(WorkspaceTickKeepsListedMember) {
    /// A unit a database lists and a default command also covers keeps
    /// its command when deleted: the tick reports the removal, not a lost
    /// command.
    TempDir tmp;
    tmp.touch("src/both.cpp", R"(int both() {})");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    project.settings.rules.push_back(
        Rule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
    project.settings.finalize(project.root);
    project.build.reset_active("");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("src/both.cpp"), {}}
    }));
    fixture.watch_databases();
    auto both = fixture.intern(tmp.path("src/both.cpp"));
    project.file_table.disk.current(both);
    ZASSERT(workspace_tick(fixture).empty());

    fs::remove_all(tmp.path("src/both.cpp"));
    auto removed = workspace_tick(fixture);
    ZASSERT(!removed.joined);
    ZASSERT(removed.changes == llvm::SmallVector<Fid>{both});
    ZASSERT(project.file_table.disk.seen_missing(both));
}

ZEST_CASE(WorkspaceTickSeesOpen) {
    /// A buffer shadows the disk for its own file's compile only: a disk
    /// change under it is still a change for everything else, reported
    /// while the file is open, once; a removal likewise.
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");
    Fixture fixture(tmp.root);
    auto& disk = fixture.server.files.disk;
    auto header = fixture.open(tmp.path("header.h"), "")->path_id;
    disk.current(header);
    fixture.watch_databases();
    ZASSERT(workspace_tick(fixture).empty());

    tmp.touch("header.h", R"(int x = 2222;)");
    auto changed = workspace_tick(fixture);
    ZASSERT(!changed.joined);
    ZASSERT(changed.changes == llvm::SmallVector<Fid>{header});
    ZASSERT(!disk.seen_missing(header));
    ZASSERT(workspace_tick(fixture).empty());

    fs::remove_all(tmp.path("header.h"));
    auto removed = workspace_tick(fixture);
    ZASSERT(!removed.joined);
    ZASSERT(removed.changes == llvm::SmallVector<Fid>{header});
    ZASSERT(disk.seen_missing(header));
}

ZEST_CASE(WorkspaceTickAfterScan) {
    /// What the load's scan read is what the first tick compares with: a
    /// rewrite arriving before that tick is still a change.
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");
    Fixture fixture(tmp.root);
    auto& disk = fixture.server.files.disk;
    auto header = fixture.intern(tmp.path("header.h"));
    disk.read(header);
    tmp.touch("header.h", R"(int x = 2222;)");
    fixture.watch_databases();

    auto first = workspace_tick(fixture);
    ZASSERT(!first.joined);
    ZASSERT(first.changes == llvm::SmallVector<Fid>{header});
    ZASSERT(!disk.seen_missing(header));
    ZASSERT(workspace_tick(fixture).empty());
}

ZEST_CASE(AnyLookReportsChange) {
    /// Whoever reads the new bytes first — here a rescan, as a database
    /// reload's graph rebuild does — reports the change; the tick after it
    /// has nothing left to report.
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");
    tmp.touch("main.cpp", R"(#include "header.h")");
    Fixture fixture(tmp.root);
    auto& project = fixture.served->project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    scan_all(project.cdb, project.toolchain, project.dep_graph);
    project.dep_graph.build_reverse_map();
    auto header = fixture.intern(tmp.path("header.h"));
    fixture.watch_databases();
    tmp.touch("header.h", R"(int x = 2222;)");
    project.rescan_disk_file(header);

    auto first = workspace_tick(fixture);
    ZASSERT(!first.joined);
    ZASSERT(first.changes.size() == 1u);
    ZASSERT(!project.file_table.disk.seen_missing(first.changes.front()));
    ZASSERT(workspace_tick(fixture).empty());
}

ZEST_CASE(CheckoutMakesWorkspaceDue) {
    /// A git operation rewrites the index: every file under the workspace
    /// falls due, and the next tick looks at it.
    TempDir tmp;
    tmp.touch(".git/HEAD", "ref: refs/heads/main\n");
    tmp.touch(".git/index", "v1");
    tmp.touch("header.h", R"(int x = 1;)");

    Fixture fixture(tmp.root);
    auto& files = fixture.server.files;
    auto time = vfs::DiskState::Clock::now();
    files.disk.now = [&] {
        return time;
    };
    auto header = fixture.intern(tmp.path("header.h"));
    files.disk.current(header);
    fixture.watch_databases();

    tmp.touch("header.h", R"(int x = 2222;)");
    files.disk.tick(std::chrono::hours(1));
    ZASSERT(files.disk.take_changes().empty());

    tmp.touch(".git/index", "v2");
    files.disk.tick(std::chrono::hours(1));
    ZASSERT(files.disk.take_changes() == llvm::SmallVector<Fid>{header});
}

ZEST_CASE(LinkedWorktreeWatched) {
    /// A linked worktree's `.git` is a file naming its git directory, where
    /// its own HEAD lives.
    TempDir tmp;
    tmp.touch("repo/.git/worktrees/wt/HEAD", "ref: refs/heads/main\n");
    tmp.touch("wt/.git", "gitdir: ../repo/.git/worktrees/wt\n");
    tmp.touch("wt/header.h", R"(int x = 1;)");

    Fixture fixture(tmp.path("wt"));
    auto& files = fixture.server.files;
    auto time = vfs::DiskState::Clock::now();
    files.disk.now = [&] {
        return time;
    };
    auto header = fixture.intern(tmp.path("wt/header.h"));
    files.disk.current(header);
    fixture.watch_databases();

    tmp.touch("wt/header.h", R"(int x = 2222;)");
    tmp.touch("repo/.git/worktrees/wt/HEAD", "ref: refs/heads/other\n");
    files.disk.tick(std::chrono::hours(1));
    ZASSERT(files.disk.take_changes() == llvm::SmallVector<Fid>{header});
}

};  // ZEST_SUITE(Databases)

}  // namespace
}  // namespace clice::testing
