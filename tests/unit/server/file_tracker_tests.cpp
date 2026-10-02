#include <chrono>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "server/file_tracker.h"
#include "vfs/path.h"

#include "llvm/Support/Process.h"

namespace clice::testing {
namespace {

/// Pin a file's modification time, as a rewrite landing within one mtime
/// tick of the previous stat leaves it.
void set_mtime(llvm::StringRef path, llvm::sys::TimePoint<> time) {
    int fd = 0;
    ASSERT(!static_cast<bool>(
        llvm::sys::fs::openFileForWrite(path, fd, llvm::sys::fs::CD_OpenExisting)));
    ASSERT(!static_cast<bool>(llvm::sys::fs::setLastAccessAndModificationTime(fd, time, time)));
    llvm::sys::Process::SafelyCloseFileDescriptor(fd);
}

/// A master tick as the tracker sees it: the file table looks at the
/// flags, then the tracker weighs what the looks found.
llvm::SmallVector<FileEvent> tick(FileTracker& tracker, FileTable& files, bool force = false) {
    files.disk.look_flags();
    return tracker.tick_cdb(force);
}

ZEST_SUITE(FileTracker) {

ZEST_CASE(CDBTickDebounces) {
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
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));

    // Rewrite with one more entry: the first tick only records the pending
    // content, the second sees it stable and reloads.
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}},
                  {tmp.root, tmp.path("lib.cpp"),  {}}
    }));
    ASSERT(tick(tracker, files).empty());

    auto events = tick(tracker, files);
    ASSERT(events.size() == 1u);
    ASSERT(events[0].kind == FileEvent::Kind::CDBChanged);
    auto lib_id = project.file_table.intern(Spelling::absolute(tmp.path("lib.cpp")));
    ASSERT(events[0].cdb.added == llvm::SmallVector<Fid>{lib_id});
    ASSERT(events[0].cdb.removed.empty());

    // Settled: further ticks are quiet.
    ASSERT(tick(tracker, files).empty());

}  // namespace

ZEST_CASE(CDBTickForceImmediate) {
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
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DFOO"}}
    }));
    auto events = tick(tracker, files, /*force=*/true);
    ASSERT(events.size() == 1u);
    auto main_id = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    ASSERT(events[0].cdb.changed == llvm::SmallVector<Fid>{main_id});
}

ZEST_CASE(CDBTickDiscoversLate) {
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");

    FileTable files;

    Project project{files};
    SessionStore store;
    // No compile_commands.json at construction time.
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    ASSERT(tick(tracker, files, /*force=*/true).empty());

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    auto events = tick(tracker, files, /*force=*/true);
    ASSERT(events.size() == 1u);
    auto main_id = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    ASSERT(events[0].cdb.added == llvm::SmallVector<Fid>{main_id});
}

ZEST_CASE(CDBTickWatchesSubdirectory) {
    /// A database generated into an existing build directory is found
    /// where the tick watches for it, and settles like a rewrite.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.mkdir("build");
    FileTable files;
    Project project{files};
    SessionStore store;
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    ASSERT(tick(tracker, files).empty());

    tmp.touch("build/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    ASSERT(tick(tracker, files).empty());
    auto events = tick(tracker, files);
    ASSERT(events.size() == 1u);
    auto main_id = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    ASSERT(events[0].cdb.added == llvm::SmallVector<Fid>{main_id});
}

ZEST_CASE(CDBTickNewSubdirectory) {
    /// A build directory created after startup is listed once the root
    /// directory moves.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    ASSERT(set_file_mtime(tmp.root, file_mtime_ns(tmp.root) - 10'000'000'000));
    FileTable files;
    Project project{files};
    SessionStore store;
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    ASSERT(tick(tracker, files).empty());

    tmp.touch("out/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    ASSERT(tick(tracker, files).empty());
    ASSERT(tick(tracker, files).size() == 1u);
}

#ifndef _WIN32
ZEST_CASE(CDBTickDanglingSymlink) {
    /// A build directory symlinked to a target created later is watched
    /// from the start.
    TempDir tmp;
    TempDir elsewhere;
    tmp.touch("main.cpp", R"(int main() {})");
    ASSERT(::symlink(elsewhere.path("target").c_str(), tmp.path("build").c_str()) == 0);
    ASSERT(set_file_mtime(tmp.root, file_mtime_ns(tmp.root) - 10'000'000'000));
    FileTable files;
    Project project{files};
    SessionStore store;
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    ASSERT(tick(tracker, files).empty());

    elsewhere.touch("target/compile_commands.json",
                    build_cdb_json({
                        {tmp.root, tmp.path("main.cpp"), {}}
    }));
    ASSERT(tick(tracker, files).empty());
    ASSERT(tick(tracker, files).size() == 1u);
}
#endif

ZEST_CASE(CDBTickAboveOpenFile) {
    /// A database generated above an open file still without a command is
    /// found where the tick watches for it.
    TempDir tmp;
    tmp.touch("a/b/main.cpp", R"(int main() {})");
    FileTable files;
    Project project{files};
    SessionStore store;
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    auto main_id = project.file_table.intern(Spelling::absolute(tmp.path("a/b/main.cpp")));
    store.open(main_id);
    ASSERT(tick(tracker, files).empty());

    tmp.touch("a/b/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/b/main.cpp"), {}}
    }));
    ASSERT(tick(tracker, files).empty());
    auto events = tick(tracker, files);
    ASSERT(events.size() == 1u);
    ASSERT(events[0].cdb.added == llvm::SmallVector<Fid>{main_id});
}

ZEST_CASE(CDBTickDeleteRecreate) {
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
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));

    // Deletion (mid-regeneration): keep serving the loaded entries.
    vfs::remove_all(tmp.path("compile_commands.json"));
    ASSERT(tick(tracker, files, /*force=*/true).empty());

    // The rewrite lands as a normal change once the file is back.
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DFOO"}}
    }));
    auto events = tick(tracker, files, /*force=*/true);
    ASSERT(events.size() == 1u);
    auto main_id = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    ASSERT(events[0].cdb.changed == llvm::SmallVector<Fid>{main_id});
}

ZEST_CASE(CDBTickRetriesFailedLoad) {
    /// A declared database unreadable at startup loads on a later tick even
    /// when its stat is unchanged by then.
    TempDir tmp;
    tmp.touch("compile_commands.json", "[ ");
    FileTable files;
    Project project{files};
    SessionStore store;
    auto id = project.cdb.add_source(Spelling::absolute(tmp.path("compile_commands.json")));
    ASSERT(!project.cdb.load_source(id).has_value());
    llvm::sys::fs::file_status before;
    ASSERT(!static_cast<bool>(llvm::sys::fs::status(tmp.path("compile_commands.json"), before)));
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));

    tmp.touch("compile_commands.json", "[]");
    int fd = 0;
    ASSERT(!static_cast<bool>(llvm::sys::fs::openFileForWrite(tmp.path("compile_commands.json"),
                                                              fd,
                                                              llvm::sys::fs::CD_OpenExisting)));
    ASSERT(!static_cast<bool>(
        llvm::sys::fs::setLastAccessAndModificationTime(fd,
                                                        before.getLastAccessedTime(),
                                                        before.getLastModificationTime())));
    llvm::sys::Process::SafelyCloseFileDescriptor(fd);

    ASSERT(tick(tracker, files).empty());
    ASSERT(tick(tracker, files).empty());
    EXPECT(project.cdb.loaded(id));
}

ZEST_CASE(CDBTickRelocates) {
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
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    auto main_id = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    auto only_id = project.file_table.intern(Spelling::absolute(tmp.path("only.cpp")));
    auto root = *project.cdb.find_source(Spelling::absolute(tmp.path("compile_commands.json")));

    vfs::remove_all(tmp.path("compile_commands.json"));
    ASSERT(tick(tracker, files, /*force=*/true).empty());
    EXPECT(!project.cdb.present(root));
    EXPECT(!project.cdb.candidate_entries(only_id).empty());

    tmp.touch("build/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DMOVED"}}
    }));
    auto events = tick(tracker, files, /*force=*/true);
    auto build =
        *project.cdb.find_source(Spelling::absolute(tmp.path("build/compile_commands.json")));
    ASSERT(events.size() == 1u);
    ASSERT(events[0].cdb.changed == llvm::SmallVector<Fid>{main_id});
    EXPECT(project.build.entries(main_id).front().source == build);
    EXPECT(project.build.entries(only_id).front().source == root);

    tmp.touch("compile_commands.json", original);
    events = tick(tracker, files, /*force=*/true);
    ASSERT(events.size() == 1u);
    ASSERT(events[0].cdb.changed == llvm::SmallVector<Fid>{main_id});
    EXPECT(project.build.entries(main_id).front().source == root);
    EXPECT(project.build.entries(main_id).size() == 2u);
}

ZEST_CASE(CDBDeletedBeforeWatch) {
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
    auto id = *project.cdb.find_source(Spelling::absolute(tmp.path("compile_commands.json")));
    ASSERT(project.cdb.present(id));
    vfs::remove_all(tmp.path("compile_commands.json"));
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    EXPECT(tick(tracker, files).empty());
    EXPECT(tick(tracker, files).empty());
    EXPECT(!project.cdb.present(id));
}

ZEST_CASE(ResponseRewriteBeforeWatch) {
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
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    EXPECT(tick(tracker, files).empty());
    auto events = tick(tracker, files);
    ASSERT(events.size() == 1u);
    auto main = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    EXPECT(events[0].cdb.changed == llvm::SmallVector<Fid>{main});
    EXPECT(tick(tracker, files).empty());
}

ZEST_CASE(ResponseAddedByReload) {
    /// A response file a reload starts reading is watched from then on.
    TempDir tmp;
    tmp.touch("main.cpp", R"(int main() {})");
    tmp.touch("flags.rsp", "-DONE\n");
    FileTable files;
    Project project{files};
    SessionStore store;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"@flags.rsp"}}
    }));
    ASSERT(tick(tracker, files, /*force=*/true).size() == 1u);

    tmp.touch("flags.rsp", "-DTWO\n");
    EXPECT(tick(tracker, files).empty());
    auto events = tick(tracker, files);
    ASSERT(events.size() == 1u);
    auto main = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    EXPECT(events[0].cdb.changed == llvm::SmallVector<Fid>{main});
}

ZEST_CASE(CDBTickRenameOver) {
    /// A same-size rewrite renamed over the database within one mtime
    /// tick is a new file: an ordinary tick sees it.
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
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    llvm::sys::fs::file_status before;
    ASSERT(!static_cast<bool>(llvm::sys::fs::status(tmp.path("compile_commands.json"), before)));

    tmp.touch("replacement.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DBBB"}}
    }));
    int fd = 0;
    ASSERT(!static_cast<bool>(llvm::sys::fs::openFileForWrite(tmp.path("replacement.json"),
                                                              fd,
                                                              llvm::sys::fs::CD_OpenExisting)));
    ASSERT(!static_cast<bool>(
        llvm::sys::fs::setLastAccessAndModificationTime(fd,
                                                        before.getLastAccessedTime(),
                                                        before.getLastModificationTime())));
    llvm::sys::Process::SafelyCloseFileDescriptor(fd);
    ASSERT(!vfs::rename(tmp.path("replacement.json"), tmp.path("compile_commands.json")));

    ASSERT(tick(tracker, files).empty());
    auto events = tick(tracker, files);
    ASSERT(events.size() == 1u);
    auto main_id = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    ASSERT(events[0].cdb.changed == llvm::SmallVector<Fid>{main_id});
}

ZEST_CASE(CDBDiscoverRetriesRegistered) {
    /// A database registered but never loaded — remembered from an earlier
    /// session, absent at startup — loads when a file under it is opened
    /// after it appeared.
    TempDir tmp;
    tmp.touch("a/main.cpp", R"(int main() {})");
    FileTable files;
    Project project{files};
    SessionStore store;
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");
    auto id = project.cdb.add_source(Spelling::absolute(tmp.path("a/compile_commands.json")));
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    auto main = project.file_table.intern(Spelling::absolute(tmp.path("a/main.cpp")));
    EXPECT(tracker.discover_around(main).empty());

    tmp.touch("a/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/main.cpp"), {}}
    }));
    auto events = tracker.discover_around(main);
    ASSERT(events.size() == 1u);
    EXPECT(events[0].cdb.added == llvm::SmallVector<Fid>{main});
    EXPECT(project.cdb.loaded(id));
    EXPECT(tracker.discover_around(main).empty());
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
    ASSERT(::symlink(tmp.path("debug.json").c_str(), database.c_str()) == 0);
    FileTable files;
    Project project{files};
    SessionStore store;
    auto id = project.cdb.add_source(Spelling::absolute(database));
    ASSERT(project.cdb.load_source(id));
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));

    vfs::remove(database);
    ASSERT(::symlink(tmp.path("release.json").c_str(), database.c_str()) == 0);
    ASSERT(tick(tracker, files).empty());
    auto events = tick(tracker, files);
    ASSERT(events.size() == 1u);
    auto main_id = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    ASSERT(events[0].cdb.changed == llvm::SmallVector<Fid>{main_id});
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
    FileTable files;
    Project project{files};
    SessionStore store;
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");
    tmp.touch("a/b/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/b/main.cpp"), {}}
    }));
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    auto main_id = project.file_table.intern(Spelling::absolute(tmp.path("a/b/main.cpp")));
    auto other_id = project.file_table.intern(Spelling::absolute(tmp.path("a/other.cpp")));

    auto events = tracker.discover_around(main_id);
    ASSERT(events.size() == 1u);
    ASSERT(events[0].cdb.added == llvm::SmallVector<Fid>{main_id});
    EXPECT(tracker.discover_around(main_id).empty());
    EXPECT(tracker.discover_around(other_id).empty());
    auto outside = project.file_table.intern(
        Spelling::absolute(path::join(path::parent_path(tmp.root), "x.cpp")));
    EXPECT(tracker.discover_around(outside).empty());

    store.open(other_id);
    tmp.touch("a/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/other.cpp"), {}}
    }));
    events = tick(tracker, files, /*force=*/true);
    ASSERT(events.size() == 1u);
    ASSERT(events[0].cdb.added == llvm::SmallVector<Fid>{other_id});
}

ZEST_CASE(CDBTickPhantomReplacement) {
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
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    auto main_id = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    auto other_id = project.file_table.intern(Spelling::absolute(tmp.path("other.cpp")));
    auto root = *project.cdb.find_source(Spelling::absolute(tmp.path("compile_commands.json")));

    vfs::remove_all(tmp.path("compile_commands.json"));
    ASSERT(tick(tracker, files, /*force=*/true).empty());
    tmp.touch("build/compile_commands.json", "not a database");
    ASSERT(tick(tracker, files, /*force=*/true).empty());

    tmp.touch("compile_commands.json", original);
    ASSERT(tick(tracker, files, /*force=*/true).empty());
    EXPECT(project.cdb.present(root));

    tmp.touch("build/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("other.cpp"), {}}
    }));
    auto events = tick(tracker, files, /*force=*/true);
    ASSERT(events.size() == 1u);
    ASSERT(events[0].cdb.added == llvm::SmallVector<Fid>{other_id});
    EXPECT(project.build.entries(main_id).size() == 1u);
    EXPECT(!project.cdb.candidate_entries(other_id).empty());
}

ZEST_CASE(CDBTickCoalescesSources) {
    /// Two databases settling in one tick make one delta.
    TempDir tmp;
    tmp.touch("a/main.cpp", R"(int main() {})");
    tmp.touch("b/other.cpp", R"(int other() {})");
    FileTable files;
    Project project{files};
    SessionStore store;
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");
    auto a = project.cdb.add_source(Spelling::absolute(tmp.path("a/compile_commands.json")));
    auto b = project.cdb.add_source(Spelling::absolute(tmp.path("b/compile_commands.json")));
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    tmp.touch("a/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("a/main.cpp"), {}}
    }));
    tmp.touch("b/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("b/other.cpp"), {}}
    }));
    EXPECT(tick(tracker, files).empty());
    auto events = tick(tracker, files);
    ASSERT(events.size() == 1u);
    EXPECT(events[0].cdb.added.size() == 2u);
    EXPECT(project.cdb.loaded(a));
    EXPECT(project.cdb.loaded(b));
}

ZEST_CASE(CDBRewriteBeforeWatch) {
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
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));

    ASSERT(tick(tracker, files).empty());
    auto events = tick(tracker, files);
    ASSERT(events.size() == 1u);
    auto main_id = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    ASSERT(events[0].cdb.changed == llvm::SmallVector<Fid>{main_id});
    ASSERT(tick(tracker, files).empty());
}

ZEST_CASE(CDBSameStampRewrite) {
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
    ASSERT(project.cdb.reload_and_diff(SourceID(0)));
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    ASSERT(tick(tracker, files).empty());

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DBBB"}}
    }));
    set_mtime(database, stamp);

    ASSERT(tick(tracker, files).empty());
    auto events = tick(tracker, files);
    ASSERT(events.size() == 1u);
    auto main_id = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    ASSERT(events[0].cdb.changed == llvm::SmallVector<Fid>{main_id});
    ASSERT(tick(tracker, files).empty());
}

ZEST_CASE(CDBForgedStampSeen) {
    /// A rewrite that puts back a size and mtime safely in the past still
    /// moves the change time: the watcher reads it and reloads.
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
    ASSERT(project.cdb.reload_and_diff(SourceID(0)));
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    ASSERT(tick(tracker, files).empty());
    ASSERT(tick(tracker, files).empty());

    // Past the coarse clock inode times are taken from.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"-DBBB"}}
    }));
    set_mtime(database, stamp);
    ASSERT(tick(tracker, files).empty());
    ASSERT(tick(tracker, files).size() == 1u);
}

/// One workspace tick of the test hook: a look at every file, the
/// build's default sources refreshed, and the disk changes those looks saw.
llvm::SmallVector<FileEvent> workspace_tick(FileTracker& tracker, FileTable& files) {
    files.disk.look_all();
    auto [ticked] = kota::run(tracker.tick_sources());
    auto events = std::move(*ticked);
    for(auto& event: take_disk_events(files)) {
        events.push_back(event);
    }
    return events;
}

ZEST_CASE(WorkspaceTickStateMachine) {
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");

    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("header.h")));
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));

    // A first look is no change.
    project.file_table.current(header);
    ASSERT(workspace_tick(tracker, files).empty());

    // Content change is confirmed by hash and reported once. The new
    // content has a different LENGTH on purpose: back-to-back writes can
    // land within one mtime tick (observed on Windows CI), and only the
    // size change keeps the stamp fast path deterministic.
    tmp.touch("header.h", R"(int x = 2222;)");
    auto changed = workspace_tick(tracker, files);
    ASSERT(changed.size() == 1u);
    ASSERT(changed[0].kind == FileEvent::Kind::DiskChanged);
    ASSERT(changed[0].path_id == header);

    // Touch: mtime may bump, identical bytes — silent either way.
    tmp.touch("header.h", R"(int x = 2222;)");
    ASSERT(workspace_tick(tracker, files).empty());

    // Removal reported once, then quiet while missing.
    vfs::remove_all(tmp.path("header.h"));
    auto removed = workspace_tick(tracker, files);
    ASSERT(removed.size() == 1u);
    ASSERT(removed[0].kind == FileEvent::Kind::DiskRemoved);
    ASSERT(removed[0].path_id == header);
    ASSERT(workspace_tick(tracker, files).empty());

    // Reappearance counts as a disk change.
    tmp.touch("header.h", R"(int x = 3;)");
    auto reborn = workspace_tick(tracker, files);
    ASSERT(reborn.size() == 1u);
    ASSERT(reborn[0].kind == FileEvent::Kind::DiskChanged);
}

ZEST_CASE(WorkspaceTickKeepsListedMember) {
    /// A unit a database lists and a default command also claims keeps
    /// its command when deleted: the tick reports the removal, not a lost
    /// command.
    TempDir tmp;
    tmp.touch("src/both.cpp", R"(int both() {})");
    FileTable files;
    Project project{files};
    SessionStore store;
    project.config.rules.push_back(
        ConfigRule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("src/both.cpp"), {}}
    }));
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    auto both = project.file_table.intern(Spelling::absolute(tmp.path("src/both.cpp")));
    project.file_table.current(both);
    ASSERT(workspace_tick(tracker, files).empty());

    vfs::remove_all(tmp.path("src/both.cpp"));
    auto removed = workspace_tick(tracker, files);
    ASSERT(removed.size() == 1u);
    ASSERT(removed[0].kind == FileEvent::Kind::DiskRemoved);
    ASSERT(removed[0].path_id == both);
}

ZEST_CASE(WorkspaceTickSeesOpen) {
    /// A buffer shadows the disk for its own file's compile only: a disk
    /// change under it is still a change for everything else, reported
    /// while the file is open, once; a removal likewise.
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");

    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("header.h")));
    store.open(header);
    project.file_table.current(header);
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    ASSERT(workspace_tick(tracker, files).empty());

    tmp.touch("header.h", R"(int x = 2222;)");
    auto changed = workspace_tick(tracker, files);
    ASSERT(changed.size() == 1u);
    ASSERT(changed[0].kind == FileEvent::Kind::DiskChanged);
    ASSERT(changed[0].path_id == header);
    ASSERT(workspace_tick(tracker, files).empty());

    vfs::remove_all(tmp.path("header.h"));
    auto removed = workspace_tick(tracker, files);
    ASSERT(removed.size() == 1u);
    ASSERT(removed[0].kind == FileEvent::Kind::DiskRemoved);
    ASSERT(removed[0].path_id == header);
}

ZEST_CASE(WorkspaceTickAfterScan) {
    /// What the load's scan read is what the first tick compares with: a
    /// rewrite landing before that tick is still a change.
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");

    FileTable files;
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("header.h")));
    project.file_table.read(header);
    tmp.touch("header.h", R"(int x = 2222;)");
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));

    auto first = workspace_tick(tracker, files);
    ASSERT(first.size() == 1u);
    ASSERT(first[0].kind == FileEvent::Kind::DiskChanged);
    ASSERT(first[0].path_id == header);
    ASSERT(workspace_tick(tracker, files).empty());
}

ZEST_CASE(AnyLookReportsChange) {
    /// Whoever reads the new bytes first — here a rescan, as a database
    /// reload's graph rebuild does — reports the change; the tick after it
    /// has nothing left to report.
    TempDir tmp;
    tmp.touch("header.h", R"(int x = 1;)");
    tmp.touch("main.cpp", R"(#include "header.h")");

    FileTable files;
    Project project{files};
    SessionStore store;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    scan_all(project.cdb, project.dep_graph);
    project.dep_graph.build_reverse_map();
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("header.h")));
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));
    tmp.touch("header.h", R"(int x = 2222;)");
    project.rescan_disk_file(header);

    auto first = workspace_tick(tracker, files);
    ASSERT(first.size() == 1u);
    ASSERT(first[0].kind == FileEvent::Kind::DiskChanged);
    ASSERT(workspace_tick(tracker, files).empty());
}

ZEST_CASE(CheckoutMakesWorkspaceDue) {
    /// A git operation rewrites the index: every file under the workspace
    /// falls due, and the next tick looks at it.
    TempDir tmp;
    tmp.touch(".git/HEAD", "ref: refs/heads/main\n");
    tmp.touch(".git/index", "v1");
    tmp.touch("header.h", R"(int x = 1;)");

    FileTable files;
    auto time = vfs::DiskState::Clock::now();
    files.disk.now = [&] {
        return time;
    };
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("header.h")));
    project.file_table.current(header);
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.root)));

    tmp.touch("header.h", R"(int x = 2222;)");
    files.disk.tick(std::chrono::hours(1));
    ASSERT(files.disk.take_changes().empty());

    tmp.touch(".git/index", "v2");
    files.disk.tick(std::chrono::hours(1));
    ASSERT(files.disk.take_changes() == llvm::SmallVector<Fid>{header});
}

ZEST_CASE(LinkedWorktreeWatched) {
    /// A linked worktree's `.git` is a file naming its git directory, where
    /// its own HEAD lives.
    TempDir tmp;
    tmp.touch("repo/.git/worktrees/wt/HEAD", "ref: refs/heads/main\n");
    tmp.touch("wt/.git", "gitdir: ../repo/.git/worktrees/wt\n");
    tmp.touch("wt/header.h", R"(int x = 1;)");

    FileTable files;
    auto time = vfs::DiskState::Clock::now();
    files.disk.now = [&] {
        return time;
    };
    Project project{files};
    SessionStore store;
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("wt/header.h")));
    project.file_table.current(header);
    FileTracker tracker(project, store, CanonicalPath(Spelling::absolute(tmp.path("wt"))));

    tmp.touch("wt/header.h", R"(int x = 2222;)");
    tmp.touch("repo/.git/worktrees/wt/HEAD", "ref: refs/heads/other\n");
    files.disk.tick(std::chrono::hours(1));
    ASSERT(files.disk.take_changes() == llvm::SmallVector<Fid>{header});
}

};  // ZEST_SUITE(FileTracker)

}  // namespace
}  // namespace clice::testing
