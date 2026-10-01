#ifndef _WIN32
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <thread>

#include "test/temp_dir.h"
#include "test/test.h"
#include "vfs/file_system.h"

#include "llvm/Support/FileSystem.h"

namespace clice::testing {

namespace {

TEST_SUITE(FileSystem) {

TEST_CASE(OpenedFilesKeepIdentity) {
    // Clang merges files by ID: a status by path and the status of the same
    // file opened, as text or as bytes, must agree on it.
    TempDir tmp;
    tmp.touch("a.h",
              "\xEF\xBB\xBF"
              "int x;\n");
    auto path = tmp.path("a.h");

    auto status = vfs::status(path);
    ASSERT_TRUE(status.has_value());
    ASSERT_EQ(status->stamp.size, 10u);

    vfs::View view;
    auto stated = view.status(path);
    auto text = view.openFileForRead(path);
    auto bytes = view.openFileForReadBinary(path);
    ASSERT_TRUE(stated && text && bytes);
    auto text_status = (*text)->status();
    auto bytes_status = (*bytes)->status();
    ASSERT_TRUE(text_status && bytes_status);

    auto id = status->to_llvm(path).getUniqueID();
    ASSERT_TRUE(stated->getUniqueID() == id);
    ASSERT_TRUE(text_status->getUniqueID() == id);
    ASSERT_TRUE(bytes_status->getUniqueID() == id);
    ASSERT_EQ(stated->getSize(), 7u);
    ASSERT_EQ(text_status->getSize(), 7u);
    ASSERT_EQ(bytes_status->getSize(), 10u);
    ASSERT_TRUE(text_status->getLastModificationTime() ==
                status->to_llvm(path).getLastModificationTime());
}

TEST_CASE(DirectoriesAndMissing) {
    TempDir tmp;
    tmp.touch("dir/a.h");

    auto dir = vfs::status(tmp.path("dir"));
    ASSERT_TRUE(dir.has_value());
    ASSERT_TRUE(dir->type == llvm::sys::fs::file_type::directory_file);
    ASSERT_EQ(dir->stamp.file, vfs::status(tmp.path("dir"))->stamp.file);
    ASSERT_NE(dir->stamp.file, vfs::status(tmp.path("dir/a.h"))->stamp.file);

    auto missing = vfs::status(tmp.path("dir/b.h"));
    ASSERT_FALSE(missing.has_value());
    ASSERT_TRUE(missing.error() == std::errc::no_such_file_or_directory);
    auto no_parent = vfs::status(tmp.path("none/b.h"));
    ASSERT_FALSE(no_parent.has_value());
    ASSERT_TRUE(no_parent.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE(LinksShareIdentity) {
    TempDir tmp;
    tmp.touch("a.h", "int x;\n");
    auto a = tmp.path("a.h");
    ASSERT_FALSE(bool(llvm::sys::fs::create_hard_link(a, tmp.path("b.h"))));
    ASSERT_TRUE(vfs::status(a)->stamp == vfs::status(tmp.path("b.h"))->stamp);
#ifndef _WIN32
    ASSERT_EQ(::symlink(a.c_str(), tmp.path("c.h").c_str()), 0);
    vfs::View view;
    auto linked = view.openFileForRead(tmp.path("c.h"));
    ASSERT_TRUE(bool(linked));
    auto id = vfs::status(a)->to_llvm(a).getUniqueID();
    ASSERT_TRUE((*linked)->status()->getUniqueID() == id);
    ASSERT_TRUE(view.status(tmp.path("c.h"))->getUniqueID() == id);
#endif
}

TEST_CASE(KeptTextFollowsEdits) {
    // A text read once is served to later compiles only while its file
    // still stats the same; an edit is read afresh.
    TempDir tmp;
    tmp.touch("a.h", "int x;\n");
    auto path = tmp.path("a.h");
    ASSERT_TRUE(set_file_mtime(path, file_mtime_ns(path) - 10'000'000'000));

    auto read = [&] {
        vfs::View view;
        auto file = view.openFileForRead(path);
        EXPECT_TRUE(bool(file));
        auto status = (*file)->status();
        EXPECT_TRUE(bool(status));
        EXPECT_TRUE(status->getUniqueID() == vfs::status(path)->to_llvm(path).getUniqueID());
        auto buffer = (*file)->getBuffer(path, -1, true, false);
        EXPECT_TRUE(bool(buffer));
        EXPECT_EQ(status->getSize(), (*buffer)->getBufferSize());
        return std::move(*buffer);
    };
    auto first = read();
    auto second = read();
    ASSERT_EQ(first->getBuffer(), "int x;\n");
    ASSERT_TRUE(first->getBufferStart() == second->getBufferStart());

    tmp.touch("a.h", "int y = 1;\n");
    ASSERT_EQ(read()->getBuffer(), "int y = 1;\n");
}

TEST_CASE(KeptMappingShared) {
    // A store artifact's mapping serves every later compile while the file
    // stats the same; any other file is mapped afresh.
    TempDir tmp;
    tmp.touch("a.pch", std::string(64 * 1024, 'x'));
    tmp.touch("b.pch", std::string(64 * 1024, 'x'));
    vfs::keep_mapped(tmp.path("a.pch"));

    auto map = [&](llvm::StringRef path) {
        vfs::View view;
        auto file = view.openFileForReadBinary(path);
        EXPECT_TRUE(bool(file));
        auto buffer = (*file)->getBuffer(path, -1, false, false);
        EXPECT_TRUE(bool(buffer));
        return std::move(*buffer);
    };
    auto a = tmp.path("a.pch");
    auto b = tmp.path("b.pch");
    ASSERT_TRUE(map(a)->getBufferStart() == map(a)->getBufferStart());
#ifdef _WIN32
    // The master and clang spell one PCH with different separators.
    auto slashed = a;
    std::ranges::replace(slashed, '\\', '/');
    ASSERT_TRUE(map(slashed)->getBufferStart() == map(a)->getBufferStart());
#endif
    ASSERT_FALSE(map(b)->getBufferStart() == map(b)->getBufferStart());
#ifndef _WIN32
    // Windows refuses to rewrite a mapped file; elsewhere a rewrite is seen.
    tmp.touch("a.pch", std::string(32 * 1024, 'y'));
    ASSERT_EQ(map(a)->getBuffer(), std::string(32 * 1024, 'y'));
#endif
}

TEST_CASE(BatchAgreesWithStatus) {
    // A directory asked about often enough answers from one listing, with
    // exactly what a status by path would say.
    TempDir tmp;
    for(int i = 0; i < 20; i += 1) {
        tmp.touch(std::format("dir/h{}.h", i), std::string(i, 'x'));
    }
    tmp.touch("dir/sub/a.h");

    vfs::StatusBatch batch;
    for(int round = 0; round < 2; round += 1) {
        for(int i = 0; i < 20; i += 1) {
            auto path = tmp.path(std::format("dir/h{}.h", i));
            auto batched = batch.status(path);
            auto direct = vfs::status(path);
            ASSERT_TRUE(batched.has_value() && direct.has_value());
            ASSERT_TRUE(batched->stamp == direct->stamp);
            ASSERT_TRUE(batched->type == direct->type);
        }
    }
    auto sub = batch.status(tmp.path("dir/sub"));
    ASSERT_TRUE(sub.has_value());
    ASSERT_TRUE(sub->type == llvm::sys::fs::file_type::directory_file);
    // A directory's times in its parent's listing may lag behind its own:
    // only its identity is compared.
    auto direct = vfs::status(tmp.path("dir/sub"));
    ASSERT_EQ(sub->stamp.device, direct->stamp.device);
    ASSERT_EQ(sub->stamp.file, direct->stamp.file);
    auto missing = batch.status(tmp.path("dir/none.h"));
    ASSERT_FALSE(missing.has_value());
    ASSERT_TRUE(missing.error() == std::errc::no_such_file_or_directory);
}

TEST_CASE(BatchSeesLinkedEdits) {
    // NTFS updates a directory entry only for the link a write went
    // through: a directory holding hard links answers by name.
    TempDir tmp;
    for(int i = 0; i < 20; i += 1) {
        tmp.touch(std::format("dir/h{}.h", i), "x");
    }
    ASSERT_FALSE(
        bool(llvm::sys::fs::create_hard_link(tmp.path("dir/h0.h"), tmp.path("dir/link.h"))));
    tmp.touch("dir/h0.h", "a longer text");

    vfs::StatusBatch batch;
    ASSERT_EQ(batch.status(tmp.path("dir/link.h"))->stamp.size, 13u);
    for(int i = 1; i < 20; i += 1) {
        ASSERT_TRUE(batch.status(tmp.path(std::format("dir/h{}.h", i))).has_value());
    }
    ASSERT_EQ(batch.status(tmp.path("dir/link.h"))->stamp.size, 13u);
}

TEST_CASE(StampSeesKeptTimes) {
    // A rewrite that keeps the size and puts the mtime back still moves
    // the change time.
    TempDir tmp;
    tmp.touch("a.h", "int x;\n");
    auto path = tmp.path("a.h");
    auto before = vfs::status(path);
    ASSERT_TRUE(before.has_value());
    // Past the coarse clock inode times are taken from.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tmp.touch("a.h", "int y;\n");
    ASSERT_TRUE(set_file_mtime(path, before->stamp.mtime_ns));
    auto after = vfs::status(path);
    ASSERT_TRUE(after.has_value());
    ASSERT_EQ(after->stamp.size, before->stamp.size);
    ASSERT_EQ(after->stamp.mtime_ns, before->stamp.mtime_ns);
    ASSERT_TRUE(after->stamp != before->stamp);
}

};  // TEST_SUITE(FileSystem)

}  // namespace

}  // namespace clice::testing
