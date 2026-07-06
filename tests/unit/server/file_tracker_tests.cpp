#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "server/state/file_tracker.h"

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
    auto lib_id = workspace.path_pool.intern(tmp.path("lib.cpp"));
    ASSERT_EQ(events[0].cdb.added, llvm::SmallVector<std::uint32_t>{lib_id});
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
    auto main_id = workspace.path_pool.intern(tmp.path("main.cpp"));
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<std::uint32_t>{main_id});
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
    auto main_id = workspace.path_pool.intern(tmp.path("main.cpp"));
    ASSERT_EQ(events[0].cdb.added, llvm::SmallVector<std::uint32_t>{main_id});
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
    auto main_id = workspace.path_pool.intern(tmp.path("main.cpp"));
    ASSERT_EQ(events[0].cdb.changed, llvm::SmallVector<std::uint32_t>{main_id});
}

TEST_CASE(WorkspaceTickStateMachine) {
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");

    kota::event_loop loop;
    Workspace workspace;
    SessionStore store;
    auto tu = workspace.path_pool.intern(tmp.path("main.cpp"));
    auto header = workspace.path_pool.intern(tmp.path("header.h"));
    workspace.dep_graph.set_includes(tu, 0, {header});
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
    auto header = workspace.path_pool.intern(tmp.path("header.h"));
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
