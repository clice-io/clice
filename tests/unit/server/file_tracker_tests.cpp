#include <chrono>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "server/file_tracker.h"
#include "support/filesystem.h"

#include "llvm/Support/Process.h"

namespace clice::testing {
namespace {

/// Pin a file's modification time, as a rewrite landing within one mtime
/// tick of the previous stat leaves it.
void set_mtime(llvm::StringRef path, llvm::sys::TimePoint<> time) {
    int fd = 0;
    ASSERT_FALSE(static_cast<bool>(
        llvm::sys::fs::openFileForWrite(path, fd, llvm::sys::fs::CD_OpenExisting)));
    ASSERT_FALSE(
        static_cast<bool>(llvm::sys::fs::setLastAccessAndModificationTime(fd, time, time)));
    llvm::sys::Process::SafelyCloseFileDescriptor(fd);
}

TEST_SUITE(FileTracker) {

TEST_CASE(CDBTickDebounces) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("lib.cpp", R"(int lib() { return 1; })");

    FileTable files;

    Project project{files};
    SessionStore store;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    FileTracker tracker(project, store, CanonicalPath(tmp.root));

    // Rewrite with one more entry: the first tick only records the pending
    // content, the second sees it stable and reloads.
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}},
                  {tmp.root, tmp.path("lib.cpp"),  {}}
    }));
    ASSERT_TRUE(tracker.tick_cdb().empty());

    auto events = tracker.tick_cdb();
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].kind, FileEvent::Kind::CDBChanged);
    auto lib_id = project.file_table.intern(tmp.path("lib.cpp"));
    ASSERT_EQ(events[0].cdb.added, llvm::SmallVector<Fid>{lib_id});
    ASSERT_TRUE(events[0].cdb.removed.empty());

    // Settled: further ticks are quiet.
    ASSERT_TRUE(tracker.tick_cdb().empty());
}

TEST_CASE(CDBTickForceImmediate) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");

    FileTable files;

    Project project{files};
    SessionStore store;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    FileTracker tracker(project, store, CanonicalPath(tmp.root));

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DFOO"}}
    }));
    auto events = tracker.tick_cdb(/*force=*/true);
    ASSERT_EQ(events.size(), 1u);
    auto main_id = project.file_table.intern(tmp.path("main.cpp"));
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
}

TEST_CASE(CDBTickDiscoversLate) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");

    FileTable files;

    Project project{files};
    SessionStore store;
    // No compile_commands.json at construction time.
    FileTracker tracker(project, store, CanonicalPath(tmp.root));
    ASSERT_TRUE(tracker.tick_cdb(/*force=*/true).empty());

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    auto events = tracker.tick_cdb(/*force=*/true);
    ASSERT_EQ(events.size(), 1u);
    auto main_id = project.file_table.intern(tmp.path("main.cpp"));
    ASSERT_EQ(events[0].cdb.added, llvm::SmallVector<Fid>{main_id});
}

TEST_CASE(CDBTickDeleteRecreate) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");

    FileTable files;

    Project project{files};
    SessionStore store;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    FileTracker tracker(project, store, CanonicalPath(tmp.root));

    // Deletion (mid-regeneration): keep serving the loaded entries.
    fs::remove_all(tmp.path("compile_commands.json"));
    ASSERT_TRUE(tracker.tick_cdb(/*force=*/true).empty());

    // The rewrite lands as a normal change once the file is back.
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DFOO"}}
    }));
    auto events = tracker.tick_cdb(/*force=*/true);
    ASSERT_EQ(events.size(), 1u);
    auto main_id = project.file_table.intern(tmp.path("main.cpp"));
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
}

TEST_CASE(CDBTickRetriesFailedLoad) {
    /// A declared database unreadable at startup loads on a later tick even
    /// when its stat is unchanged by then.
    TempDir tmp;
    tmp.touch("compile_commands.json", "[ ");
    FileTable files;
    Project project{files};
    SessionStore store;
    auto id = project.cdb.add_source(tmp.path("compile_commands.json"));
    ASSERT_FALSE(project.cdb.load_source(id).has_value());
    llvm::sys::fs::file_status before;
    ASSERT_FALSE(
        static_cast<bool>(llvm::sys::fs::status(tmp.path("compile_commands.json"), before)));
    FileTracker tracker(project, store, CanonicalPath(tmp.root));

    tmp.touch("compile_commands.json", "[]");
    int fd = 0;
    ASSERT_FALSE(
        static_cast<bool>(llvm::sys::fs::openFileForWrite(tmp.path("compile_commands.json"),
                                                          fd,
                                                          llvm::sys::fs::CD_OpenExisting)));
    ASSERT_FALSE(static_cast<bool>(
        llvm::sys::fs::setLastAccessAndModificationTime(fd,
                                                        before.getLastAccessedTime(),
                                                        before.getLastModificationTime())));
    llvm::sys::Process::SafelyCloseFileDescriptor(fd);

    ASSERT_TRUE(tracker.tick_cdb().empty());
    ASSERT_TRUE(tracker.tick_cdb().empty());
    EXPECT_TRUE(project.cdb.loaded(id));
}

TEST_CASE(CDBTickRelocates) {
    /// The discovered database is deleted and one appears elsewhere: the
    /// old entries keep serving, the new database loads through the usual
    /// path, and the files both list change command — the present
    /// database ranks first — until the original returns.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("only.cpp", R"(int only() {})");

    FileTable files;

    Project project{files};
    SessionStore store;
    auto original = build_cdb_json({
        {tmp.root, tmp.path("main.cpp"), {}},
        {tmp.root, tmp.path("only.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, original);
    FileTracker tracker(project, store, CanonicalPath(tmp.root));
    auto main_id = project.file_table.intern(tmp.path("main.cpp"));
    auto only_id = project.file_table.intern(tmp.path("only.cpp"));
    auto root = *project.cdb.find_source(tmp.path("compile_commands.json"));

    fs::remove_all(tmp.path("compile_commands.json"));
    ASSERT_TRUE(tracker.tick_cdb(/*force=*/true).empty());
    EXPECT_FALSE(project.cdb.present(root));
    EXPECT_FALSE(project.cdb.candidate_entries(only_id).empty());

    tmp.touch("build/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DMOVED"}}
    }));
    auto events = tracker.tick_cdb(/*force=*/true);
    auto build = *project.cdb.find_source(tmp.path("build/compile_commands.json"));
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
    EXPECT_EQ(project.build.entries(main_id).front().source, build);
    EXPECT_EQ(project.build.entries(only_id).front().source, root);

    tmp.touch("compile_commands.json", original);
    events = tracker.tick_cdb(/*force=*/true);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
    EXPECT_EQ(project.build.entries(main_id).front().source, root);
    EXPECT_EQ(project.build.entries(main_id).size(), 2u);
}

TEST_CASE(CDBDeletedBeforeWatch) {
    /// A database loaded, then deleted before the tracker watches it: the
    /// load's read is the baseline, so the deletion settles like any other.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    FileTable files;
    Project project{files};
    SessionStore store;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    auto id = *project.cdb.find_source(tmp.path("compile_commands.json"));
    ASSERT_TRUE(project.cdb.present(id));
    fs::remove_all(tmp.path("compile_commands.json"));
    FileTracker tracker(project, store, CanonicalPath(tmp.root));
    EXPECT_TRUE(tracker.tick_cdb().empty());
    EXPECT_TRUE(tracker.tick_cdb().empty());
    EXPECT_FALSE(project.cdb.present(id));
}

TEST_CASE(ResponseRewriteBeforeWatch) {
    /// A response file rewritten between the startup load and the watch:
    /// the load's own read is the baseline, so the flags in memory catch
    /// up.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("flags.rsp", "-DONE\n");
    FileTable files;
    Project project{files};
    SessionStore store;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"@flags.rsp"}}
    }));
    tmp.touch("flags.rsp", "-DTWO\n");
    FileTracker tracker(project, store, CanonicalPath(tmp.root));
    EXPECT_TRUE(tracker.tick_cdb().empty());
    auto events = tracker.tick_cdb();
    ASSERT_EQ(events.size(), 1u);
    auto main = project.file_table.intern(tmp.path("main.cpp"));
    EXPECT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main});
    EXPECT_TRUE(tracker.tick_cdb().empty());
}

TEST_CASE(CDBTickRenameOver) {
    /// A same-size rewrite renamed over the database within one mtime
    /// tick is a new file: an ordinary tick sees it where stable file
    /// identities exist.
    if constexpr(fs::stable_file_ids) {
        TempDir tmp;
        tmp.touch("main.cpp", R"(int main() {})");
        FileTable files;
        Project project{files};
        SessionStore store;
        write_cdb(tmp,
                  project.cdb,
                  build_cdb_json({
                      {tmp.root, tmp.path("main.cpp"), {"-DAAA"}}
        }));
        FileTracker tracker(project, store, CanonicalPath(tmp.root));
        llvm::sys::fs::file_status before;
        ASSERT_FALSE(
            static_cast<bool>(llvm::sys::fs::status(tmp.path("compile_commands.json"), before)));

        tmp.touch("replacement.json",
                  build_cdb_json({
                      {tmp.root, tmp.path("main.cpp"), {"-DBBB"}}
        }));
        int fd = 0;
        ASSERT_FALSE(
            static_cast<bool>(llvm::sys::fs::openFileForWrite(tmp.path("replacement.json"),
                                                              fd,
                                                              llvm::sys::fs::CD_OpenExisting)));
        ASSERT_FALSE(static_cast<bool>(
            llvm::sys::fs::setLastAccessAndModificationTime(fd,
                                                            before.getLastAccessedTime(),
                                                            before.getLastModificationTime())));
        llvm::sys::Process::SafelyCloseFileDescriptor(fd);
        ASSERT_TRUE(fs::rename(tmp.path("replacement.json"), tmp.path("compile_commands.json"))
                        .has_value());

        ASSERT_TRUE(tracker.tick_cdb().empty());
        auto events = tracker.tick_cdb();
        ASSERT_EQ(events.size(), 1u);
        auto main_id = project.file_table.intern(tmp.path("main.cpp"));
        ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
    }
}

TEST_CASE(CDBDiscoverRetriesRegistered) {
    /// A database registered but never loaded — remembered from an earlier
    /// session, absent at startup — loads when a file under it is opened
    /// after it appeared.
    TempDir tmp;
    tmp.touch("a/main.cpp", R"(int main() {})");
    FileTable files;
    Project project{files};
    SessionStore store;
    project.config.finalize(tmp.root.str());
    project.build.reset_active("");
    auto id = project.cdb.add_source(tmp.path("a/compile_commands.json"));
    FileTracker tracker(project, store, CanonicalPath(tmp.root));
    auto main = project.file_table.intern(tmp.path("a/main.cpp"));
    EXPECT_TRUE(tracker.discover_around(main).empty());

    tmp.touch("a/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/main.cpp"), {}}
    }));
    auto events = tracker.discover_around(main);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].cdb.added, llvm::SmallVector<Fid>{main});
    EXPECT_TRUE(project.cdb.loaded(id));
    EXPECT_TRUE(tracker.discover_around(main).empty());
}

#ifndef _WIN32
TEST_CASE(CDBTickFollowsRetarget) {
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
    ASSERT_EQ(::symlink(tmp.path("debug.json").c_str(), database.c_str()), 0);
    FileTable files;
    Project project{files};
    SessionStore store;
    auto id = project.cdb.add_source(database);
    ASSERT_TRUE(project.cdb.load_source(id).has_value());
    FileTracker tracker(project, store, CanonicalPath(tmp.root));

    fs::remove(database);
    ASSERT_EQ(::symlink(tmp.path("release.json").c_str(), database.c_str()), 0);
    ASSERT_TRUE(tracker.tick_cdb().empty());
    auto events = tracker.tick_cdb();
    ASSERT_EQ(events.size(), 1u);
    auto main_id = project.file_table.intern(tmp.path("main.cpp"));
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
}
#endif

TEST_CASE(CDBTickDiscoversAround) {
    /// Opening a file registers the databases above it up to the root, at
    /// once; a file with a command, or outside the workspace, registers
    /// nothing, and a database above a file still without a command is
    /// found by a later tick.
    TempDir tmp;
    tmp.touch("a/b/main.cpp", R"(int main() {})");
    tmp.touch("a/other.cpp", R"(int other() {})");
    FileTable files;
    Project project{files};
    SessionStore store;
    project.config.finalize(tmp.root.str());
    project.build.reset_active("");
    tmp.touch("a/b/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/b/main.cpp"), {}}
    }));
    FileTracker tracker(project, store, CanonicalPath(tmp.root));
    auto main_id = project.file_table.intern(tmp.path("a/b/main.cpp"));
    auto other_id = project.file_table.intern(tmp.path("a/other.cpp"));

    auto events = tracker.discover_around(main_id);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].cdb.added, llvm::SmallVector<Fid>{main_id});
    EXPECT_TRUE(tracker.discover_around(main_id).empty());
    EXPECT_TRUE(tracker.discover_around(other_id).empty());
    auto outside = project.file_table.intern(path::join(path::parent_path(tmp.root), "x.cpp"));
    EXPECT_TRUE(tracker.discover_around(outside).empty());

    store.open(other_id);
    tmp.touch("a/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/other.cpp"), {}}
    }));
    events = tracker.tick_cdb(/*force=*/true);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].cdb.added, llvm::SmallVector<Fid>{other_id});
}

TEST_CASE(CDBTickPhantomReplacement) {
    /// A replacement that does not parse appears while the original is
    /// gone, the original comes back, then the replacement is repaired:
    /// nothing ever left, and the repaired database only adds what the
    /// original lacks.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("other.cpp", R"(int other() {})");
    FileTable files;
    Project project{files};
    SessionStore store;
    auto original = build_cdb_json({
        {tmp.root, tmp.path("main.cpp"), {}}
    });
    write_cdb(tmp, project.cdb, original);
    FileTracker tracker(project, store, CanonicalPath(tmp.root));
    auto main_id = project.file_table.intern(tmp.path("main.cpp"));
    auto other_id = project.file_table.intern(tmp.path("other.cpp"));
    auto root = *project.cdb.find_source(tmp.path("compile_commands.json"));

    fs::remove_all(tmp.path("compile_commands.json"));
    ASSERT_TRUE(tracker.tick_cdb(/*force=*/true).empty());
    tmp.touch("build/compile_commands.json", "not a database");
    ASSERT_TRUE(tracker.tick_cdb(/*force=*/true).empty());

    tmp.touch("compile_commands.json", original);
    ASSERT_TRUE(tracker.tick_cdb(/*force=*/true).empty());
    EXPECT_TRUE(project.cdb.present(root));

    tmp.touch("build/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("other.cpp"), {}}
    }));
    auto events = tracker.tick_cdb(/*force=*/true);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].cdb.added, llvm::SmallVector<Fid>{other_id});
    EXPECT_EQ(project.build.entries(main_id).size(), 1u);
    EXPECT_FALSE(project.cdb.candidate_entries(other_id).empty());
}

TEST_CASE(CDBTickCoalescesSources) {
    /// Two databases settling in one tick make one delta.
    TempDir tmp;
    tmp.touch("a/main.cpp", R"(int main() {})");
    tmp.touch("b/other.cpp", R"(int other() {})");
    FileTable files;
    Project project{files};
    SessionStore store;
    project.config.finalize(tmp.root.str());
    project.build.reset_active("");
    auto a = project.cdb.add_source(tmp.path("a/compile_commands.json"));
    auto b = project.cdb.add_source(tmp.path("b/compile_commands.json"));
    FileTracker tracker(project, store, CanonicalPath(tmp.root));
    tmp.touch("a/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/main.cpp"), {}}
    }));
    tmp.touch("b/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("b/other.cpp"), {}}
    }));
    EXPECT_TRUE(tracker.tick_cdb().empty());
    auto events = tracker.tick_cdb();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].cdb.added.size(), 2u);
    EXPECT_TRUE(project.cdb.loaded(a));
    EXPECT_TRUE(project.cdb.loaded(b));
}

TEST_CASE(CDBRewriteBeforeWatch) {
    /// A rewrite landing between the load and the watch is a change: the
    /// baseline is the load's own read, not a stat taken afterwards.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    FileTable files;
    Project project{files};
    SessionStore store;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DAAA"}}
    }));
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DLONGER"}}
    }));
    FileTracker tracker(project, store, CanonicalPath(tmp.root));

    ASSERT_TRUE(tracker.tick_cdb().empty());
    auto events = tracker.tick_cdb();
    ASSERT_EQ(events.size(), 1u);
    auto main_id = project.file_table.intern(tmp.path("main.cpp"));
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
    ASSERT_TRUE(tracker.tick_cdb().empty());
}

TEST_CASE(CDBSameStampRewrite) {
    /// A same-size rewrite in place within the mtime granularity of the
    /// load leaves the stat untouched: the stat of a fresh load cannot
    /// vouch for the bytes, so the ticks compare the content.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    FileTable files;
    Project project{files};
    SessionStore store;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DAAA"}}
    }));
    // An mtime not yet safely in the past, however long the load takes.
    auto database = tmp.path("compile_commands.json");
    auto stamp = std::chrono::system_clock::now() + std::chrono::hours(1);
    set_mtime(database, stamp);
    ASSERT_TRUE(project.cdb.reload_and_diff(SourceID(0)).has_value());
    FileTracker tracker(project, store, CanonicalPath(tmp.root));
    ASSERT_TRUE(tracker.tick_cdb().empty());

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DBBB"}}
    }));
    set_mtime(database, stamp);

    ASSERT_TRUE(tracker.tick_cdb().empty());
    auto events = tracker.tick_cdb();
    ASSERT_EQ(events.size(), 1u);
    auto main_id = project.file_table.intern(tmp.path("main.cpp"));
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
    ASSERT_TRUE(tracker.tick_cdb().empty());
}

TEST_CASE(CDBTrustedStampQuiet) {
    /// A stat safely in the past vouches for the bytes: the watcher reads
    /// nothing while it holds, so a rewrite forging it goes unseen — the
    /// stat polling's accepted blind spot.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    FileTable files;
    Project project{files};
    SessionStore store;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DAAA"}}
    }));
    auto database = tmp.path("compile_commands.json");
    auto stamp = std::chrono::system_clock::now() - std::chrono::hours(1);
    set_mtime(database, stamp);
    ASSERT_TRUE(project.cdb.reload_and_diff(SourceID(0)).has_value());
    FileTracker tracker(project, store, CanonicalPath(tmp.root));
    ASSERT_TRUE(tracker.tick_cdb().empty());
    ASSERT_TRUE(tracker.tick_cdb().empty());

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DBBB"}}
    }));
    set_mtime(database, stamp);
    ASSERT_TRUE(tracker.tick_cdb().empty());
    ASSERT_TRUE(tracker.tick_cdb().empty());
}

/// One workspace sweep and the disk changes the file table saw during it.
kota::task<llvm::SmallVector<FileEvent>> sweep(FileTracker& tracker, FileTable& files) {
    auto events = co_await tracker.tick_workspace();
    for(auto& event: take_disk_events(files)) {
        events.push_back(event);
    }
    co_return events;
}

TEST_CASE(WorkspaceTickStateMachine) {
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");

    kota::event_loop loop;
    FileTable files;
    Project project{files};
    SessionStore store;
    auto tu = project.file_table.intern(tmp.path("main.cpp"));
    auto header = project.file_table.intern(tmp.path("header.h"));
    project.dep_graph.set_includes(tu, 0, {{header}});
    project.dep_graph.build_reverse_map();
    FileTracker tracker(project, store, CanonicalPath(tmp.root));

    auto body = [&]() -> kota::task<> {
        // A first look is no change, even for main.cpp missing on disk.
        auto seeded = co_await sweep(tracker, files);
        EXPECT_TRUE(seeded.empty());

        // Content change is confirmed by hash and reported once. The new
        // content has a different LENGTH on purpose: back-to-back writes
        // can land within one mtime tick (observed on Windows CI), and only
        // the size change keeps the (mtime, size) fast path deterministic.
        // (ASSERT_* expands to `return` and cannot be used in coroutines.)
        tmp.touch("header.h", R"(int x = 2222;)");
        auto changed = co_await sweep(tracker, files);
        EXPECT_EQ(changed.size(), 1u);
        if(changed.size() == 1) {
            EXPECT_EQ(changed[0].kind, FileEvent::Kind::DiskChanged);
            EXPECT_EQ(changed[0].path_id, header);
        }

        // Touch: mtime may bump, identical bytes — silent either way.
        tmp.touch("header.h", R"(int x = 2222;)");
        auto touched = co_await sweep(tracker, files);
        EXPECT_TRUE(touched.empty());

        // Removal reported once, then quiet while missing.
        fs::remove_all(tmp.path("header.h"));
        auto removed = co_await sweep(tracker, files);
        EXPECT_EQ(removed.size(), 1u);
        if(removed.size() == 1) {
            EXPECT_EQ(removed[0].kind, FileEvent::Kind::DiskRemoved);
            EXPECT_EQ(removed[0].path_id, header);
        }
        auto still_removed = co_await sweep(tracker, files);
        EXPECT_TRUE(still_removed.empty());

        // Reappearance counts as a disk change.
        tmp.touch("header.h", R"(int x = 3;)");
        auto reborn = co_await sweep(tracker, files);
        EXPECT_EQ(reborn.size(), 1u);
        if(reborn.size() == 1) {
            EXPECT_EQ(reborn[0].kind, FileEvent::Kind::DiskChanged);
        }
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
}

TEST_CASE(WorkspaceTickKeepsListedMember) {
    /// A unit a database lists and a default command also claims keeps
    /// its command when deleted: the sweep reports the removal, not a
    /// lost command.
    TempDir tmp;
    tmp.touch("src/both.cpp", R"(int both() {})");
    kota::event_loop loop;
    FileTable files;
    Project project{files};
    SessionStore store;
    project.config.rules.push_back(
        ConfigRule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
    project.config.finalize(tmp.root.str());
    project.build.reset_active("");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("src/both.cpp"), {}}
    }));
    FileTracker tracker(project, store, CanonicalPath(tmp.root));
    auto both = project.file_table.intern(tmp.path("src/both.cpp"));
    project.dep_graph.set_includes(both, 0, {});
    project.dep_graph.build_reverse_map();
    auto body = [&]() -> kota::task<> {
        auto seeded = co_await sweep(tracker, files);
        EXPECT_TRUE(seeded.empty());
        fs::remove_all(tmp.path("src/both.cpp"));
        auto removed = co_await sweep(tracker, files);
        EXPECT_EQ(removed.size(), 1u);
        if(removed.size() == 1) {
            EXPECT_EQ(removed[0].kind, FileEvent::Kind::DiskRemoved);
            EXPECT_EQ(removed[0].path_id, both);
        }
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
}

TEST_CASE(WorkspaceTickSeesOpen) {
    /// A buffer shadows the disk for its own file's compile only: a disk
    /// change under it is still a change for everything else, reported
    /// while the file is open, once; a removal likewise.
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");

    kota::event_loop loop;
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern(tmp.path("header.h"));
    project.dep_graph.set_includes(header, 0, {});
    project.dep_graph.build_reverse_map();
    store.open(header);
    FileTracker tracker(project, store, CanonicalPath(tmp.root));

    auto body = [&]() -> kota::task<> {
        EXPECT_TRUE((co_await sweep(tracker, files)).empty());

        tmp.touch("header.h", R"(int x = 2222;)");
        auto changed = co_await sweep(tracker, files);
        EXPECT_EQ(changed.size(), 1u);
        if(changed.size() == 1) {
            EXPECT_EQ(changed[0].kind, FileEvent::Kind::DiskChanged);
            EXPECT_EQ(changed[0].path_id, header);
        }
        EXPECT_TRUE((co_await sweep(tracker, files)).empty());

        fs::remove_all(tmp.path("header.h"));
        auto removed = co_await sweep(tracker, files);
        EXPECT_EQ(removed.size(), 1u);
        if(removed.size() == 1) {
            EXPECT_EQ(removed[0].kind, FileEvent::Kind::DiskRemoved);
            EXPECT_EQ(removed[0].path_id, header);
        }
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
}

TEST_CASE(WorkspaceTickAfterScan) {
    /// What the load's scan read is what the first sweep compares with: a
    /// rewrite landing before that sweep is still a change.
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");

    kota::event_loop loop;
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern(tmp.path("header.h"));
    project.dep_graph.set_includes(header, 0, {});
    project.dep_graph.build_reverse_map();
    project.file_table.read(header);
    tmp.touch("header.h", R"(int x = 2222;)");
    FileTracker tracker(project, store, CanonicalPath(tmp.root));

    auto body = [&]() -> kota::task<> {
        auto first = co_await sweep(tracker, files);
        EXPECT_EQ(first.size(), 1u);
        if(first.size() == 1) {
            EXPECT_EQ(first[0].kind, FileEvent::Kind::DiskChanged);
            EXPECT_EQ(first[0].path_id, header);
        }
        EXPECT_TRUE((co_await sweep(tracker, files)).empty());
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
}

TEST_CASE(AnyLookReportsChange) {
    /// Whoever reads the new bytes first — here a rescan, as a database
    /// reload's graph rebuild does — reports the change; the sweep after
    /// it has nothing left to report.
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");

    kota::event_loop loop;
    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern(tmp.path("header.h"));
    project.dep_graph.set_includes(header, 0, {});
    project.dep_graph.build_reverse_map();
    project.file_table.read(header);
    FileTracker tracker(project, store, CanonicalPath(tmp.root));
    tmp.touch("header.h", R"(int x = 2222;)");
    project.rescan_disk_file(header);

    auto body = [&]() -> kota::task<> {
        auto first = co_await sweep(tracker, files);
        EXPECT_EQ(first.size(), 1u);
        if(first.size() == 1) {
            EXPECT_EQ(first[0].kind, FileEvent::Kind::DiskChanged);
        }
        EXPECT_TRUE((co_await sweep(tracker, files)).empty());
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
}

};  // TEST_SUITE(FileTracker)

}  // namespace
}  // namespace clice::testing
