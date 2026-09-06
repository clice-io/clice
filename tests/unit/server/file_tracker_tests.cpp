#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "server/state/file_tracker.h"
#include "support/filesystem.h"

#include "llvm/Support/Process.h"

namespace clice::testing {
namespace {

TEST_SUITE(FileTracker) {

TEST_CASE(CDBTickDebounces) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("lib.cpp", R"(int lib() { return 1; })");

    Workspace workspace;
    SessionStore store;
    write_cdb(tmp,
              workspace.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    FileTracker tracker(workspace, store, tmp.root.str().str());

    // Rewrite with one more entry: the first tick only records the pending
    // stamp, the second sees it stable and reloads.
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}},
                  {tmp.root, tmp.path("lib.cpp"),  {}}
    }));
    ASSERT_TRUE(tracker.tick_cdb().empty());

    auto events = tracker.tick_cdb();
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].kind, FileEvent::Kind::CDBChanged);
    auto lib_id = workspace.file_table.intern(tmp.path("lib.cpp"));
    ASSERT_EQ(events[0].cdb.added, llvm::SmallVector<Fid>{lib_id});
    ASSERT_TRUE(events[0].cdb.removed.empty());

    // Settled: further ticks are quiet.
    ASSERT_TRUE(tracker.tick_cdb().empty());
}

TEST_CASE(CDBTickForceImmediate) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");

    Workspace workspace;
    SessionStore store;
    write_cdb(tmp,
              workspace.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    FileTracker tracker(workspace, store, tmp.root.str().str());

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DFOO"}}
    }));
    auto events = tracker.tick_cdb(/*force=*/true);
    ASSERT_EQ(events.size(), 1u);
    auto main_id = workspace.file_table.intern(tmp.path("main.cpp"));
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
}

TEST_CASE(CDBTickDiscoversLate) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");

    Workspace workspace;
    SessionStore store;
    // No compile_commands.json at construction time.
    FileTracker tracker(workspace, store, tmp.root.str().str());
    ASSERT_TRUE(tracker.tick_cdb(/*force=*/true).empty());

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    auto events = tracker.tick_cdb(/*force=*/true);
    ASSERT_EQ(events.size(), 1u);
    auto main_id = workspace.file_table.intern(tmp.path("main.cpp"));
    ASSERT_EQ(events[0].cdb.added, llvm::SmallVector<Fid>{main_id});
}

TEST_CASE(CDBTickDeleteRecreate) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");

    Workspace workspace;
    SessionStore store;
    write_cdb(tmp,
              workspace.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    FileTracker tracker(workspace, store, tmp.root.str().str());

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
    auto main_id = workspace.file_table.intern(tmp.path("main.cpp"));
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
}

TEST_CASE(CDBTickRetriesFailedLoad) {
    /// A declared database unreadable at startup loads on a later tick even
    /// when its stamp is unchanged by then.
    TempDir tmp;
    tmp.touch("compile_commands.json", "[ ");
    Workspace workspace;
    SessionStore store;
    auto id = workspace.cdb.add_source(tmp.path("compile_commands.json"));
    ASSERT_FALSE(workspace.cdb.load_source(id).has_value());
    llvm::sys::fs::file_status before;
    ASSERT_FALSE(
        static_cast<bool>(llvm::sys::fs::status(tmp.path("compile_commands.json"), before)));
    FileTracker tracker(workspace, store, tmp.root.str().str());

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
    EXPECT_TRUE(workspace.cdb.loaded(id));
}

TEST_CASE(CDBTickRelocates) {
    /// The discovered database is deleted and one appears elsewhere: the
    /// old entries keep serving, the new database loads through the usual
    /// path, and the files both list change command — the present
    /// database ranks first — until the original returns.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("only.cpp", R"(int only() {})");

    Workspace workspace;
    SessionStore store;
    auto original = build_cdb_json({
        {tmp.root, tmp.path("main.cpp"), {}},
        {tmp.root, tmp.path("only.cpp"), {}}
    });
    write_cdb(tmp, workspace.cdb, original);
    FileTracker tracker(workspace, store, tmp.root.str().str());
    auto main_id = workspace.file_table.intern(tmp.path("main.cpp"));
    auto only_id = workspace.file_table.intern(tmp.path("only.cpp"));
    auto root = *workspace.cdb.find_source(tmp.path("compile_commands.json"));

    fs::remove_all(tmp.path("compile_commands.json"));
    ASSERT_TRUE(tracker.tick_cdb(/*force=*/true).empty());
    EXPECT_FALSE(workspace.cdb.present(root));
    EXPECT_FALSE(workspace.cdb.candidate_entries(only_id).empty());

    tmp.touch("build/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DMOVED"}}
    }));
    auto events = tracker.tick_cdb(/*force=*/true);
    auto build = *workspace.cdb.find_source(tmp.path("build/compile_commands.json"));
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
    EXPECT_EQ(workspace.build.entries(main_id).front().source, build);
    EXPECT_EQ(workspace.build.entries(only_id).front().source, root);

    tmp.touch("compile_commands.json", original);
    events = tracker.tick_cdb(/*force=*/true);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
    EXPECT_EQ(workspace.build.entries(main_id).front().source, root);
    EXPECT_EQ(workspace.build.entries(main_id).size(), 2u);
}

TEST_CASE(CDBTickRenameOver) {
    /// A same-size rewrite renamed over the database within one mtime
    /// tick is a new file: an ordinary tick sees it where stable file
    /// identities exist.
    if constexpr(fs::stable_file_ids) {
        TempDir tmp;
        tmp.touch("main.cpp", R"(int main() {})");
        Workspace workspace;
        SessionStore store;
        write_cdb(tmp,
                  workspace.cdb,
                  build_cdb_json({
                      {tmp.root, tmp.path("main.cpp"), {"-DAAA"}}
        }));
        FileTracker tracker(workspace, store, tmp.root.str().str());
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
        auto main_id = workspace.file_table.intern(tmp.path("main.cpp"));
        ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<Fid>{main_id});
    }
}

TEST_CASE(CDBTickDiscoversAround) {
    /// Opening a file registers the databases above it up to the root, at
    /// once; a file with a command, or outside the workspace, registers
    /// nothing, and a database above a file still without a command is
    /// found by a later tick.
    TempDir tmp;
    tmp.touch("a/b/main.cpp", R"(int main() {})");
    tmp.touch("a/other.cpp", R"(int other() {})");
    Workspace workspace;
    SessionStore store;
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");
    tmp.touch("a/b/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/b/main.cpp"), {}}
    }));
    FileTracker tracker(workspace, store, tmp.root.str().str());
    auto main_id = workspace.file_table.intern(tmp.path("a/b/main.cpp"));
    auto other_id = workspace.file_table.intern(tmp.path("a/other.cpp"));

    auto events = tracker.discover_around(main_id);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].cdb.added, llvm::SmallVector<Fid>{main_id});
    EXPECT_TRUE(tracker.discover_around(main_id).empty());
    EXPECT_TRUE(tracker.discover_around(other_id).empty());
    auto outside = workspace.file_table.intern(path::join(path::parent_path(tmp.root), "x.cpp"));
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
    Workspace workspace;
    SessionStore store;
    auto original = build_cdb_json({
        {tmp.root, tmp.path("main.cpp"), {}}
    });
    write_cdb(tmp, workspace.cdb, original);
    FileTracker tracker(workspace, store, tmp.root.str().str());
    auto main_id = workspace.file_table.intern(tmp.path("main.cpp"));
    auto other_id = workspace.file_table.intern(tmp.path("other.cpp"));
    auto root = *workspace.cdb.find_source(tmp.path("compile_commands.json"));

    fs::remove_all(tmp.path("compile_commands.json"));
    ASSERT_TRUE(tracker.tick_cdb(/*force=*/true).empty());
    tmp.touch("build/compile_commands.json", "not a database");
    ASSERT_TRUE(tracker.tick_cdb(/*force=*/true).empty());

    tmp.touch("compile_commands.json", original);
    ASSERT_TRUE(tracker.tick_cdb(/*force=*/true).empty());
    EXPECT_TRUE(workspace.cdb.present(root));

    tmp.touch("build/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("other.cpp"), {}}
    }));
    auto events = tracker.tick_cdb(/*force=*/true);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].cdb.added, llvm::SmallVector<Fid>{other_id});
    EXPECT_EQ(workspace.build.entries(main_id).size(), 1u);
    EXPECT_FALSE(workspace.cdb.candidate_entries(other_id).empty());
}

TEST_CASE(WorkspaceTickStateMachine) {
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");

    kota::event_loop loop;
    Workspace workspace;
    SessionStore store;
    auto tu = workspace.file_table.intern(tmp.path("main.cpp"));
    auto header = workspace.file_table.intern(tmp.path("header.h"));
    workspace.dep_graph.set_includes(tu, 0, {{header}});
    workspace.dep_graph.build_reverse_map();
    FileTracker tracker(workspace, store, tmp.root.str().str());

    auto body = [&]() -> kota::task<> {
        // First sweep seeds the baseline silently, even though main.cpp is
        // missing on disk.
        auto seeded = co_await tracker.tick_workspace();
        EXPECT_TRUE(seeded.empty());

        // Content change is confirmed by hash and reported once. The new
        // content has a different LENGTH on purpose: back-to-back writes
        // can land within one mtime tick (observed on Windows CI), and only
        // the size change keeps the (mtime, size) fast path deterministic.
        // (ASSERT_* expands to `return` and cannot be used in coroutines.)
        tmp.touch("header.h", R"(int x = 2222;)");
        auto changed = co_await tracker.tick_workspace();
        EXPECT_EQ(changed.size(), 1u);
        if(changed.size() == 1) {
            EXPECT_EQ(changed[0].kind, FileEvent::Kind::DiskChanged);
            EXPECT_EQ(changed[0].path_id, header);
        }

        // Touch: mtime may bump, identical bytes — silent either way.
        tmp.touch("header.h", R"(int x = 2222;)");
        auto touched = co_await tracker.tick_workspace();
        EXPECT_TRUE(touched.empty());

        // Removal reported once, then quiet while missing.
        fs::remove_all(tmp.path("header.h"));
        auto removed = co_await tracker.tick_workspace();
        EXPECT_EQ(removed.size(), 1u);
        if(removed.size() == 1) {
            EXPECT_EQ(removed[0].kind, FileEvent::Kind::DiskRemoved);
            EXPECT_EQ(removed[0].path_id, header);
        }
        auto still_removed = co_await tracker.tick_workspace();
        EXPECT_TRUE(still_removed.empty());

        // Reappearance counts as a disk change.
        tmp.touch("header.h", R"(int x = 3;)");
        auto reborn = co_await tracker.tick_workspace();
        EXPECT_EQ(reborn.size(), 1u);
        if(reborn.size() == 1) {
            EXPECT_EQ(reborn[0].kind, FileEvent::Kind::DiskChanged);
        }
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
}

TEST_CASE(WorkspaceTickSkipsOpen) {
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");

    kota::event_loop loop;
    Workspace workspace;
    SessionStore store;
    auto header = workspace.file_table.intern(tmp.path("header.h"));
    workspace.dep_graph.set_includes(header, 0, {});
    workspace.dep_graph.build_reverse_map();
    store.open(header);
    FileTracker tracker(workspace, store, tmp.root.str().str());

    auto body = [&]() -> kota::task<> {
        EXPECT_TRUE((co_await tracker.tick_workspace()).empty());

        // The open buffer is the truth: its disk changes are not tracked.
        tmp.touch("header.h", R"(int x = 2;)");
        EXPECT_TRUE((co_await tracker.tick_workspace()).empty());
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
}

};  // TEST_SUITE(FileTracker)

}  // namespace
}  // namespace clice::testing
