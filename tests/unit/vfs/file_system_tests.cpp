#ifndef _WIN32
#include <unistd.h>
#endif

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
    ASSERT_EQ(status->getSize(), 10u);

    vfs::View view;
    auto stated = view.status(path);
    auto text = view.openFileForRead(path);
    auto bytes = view.openFileForReadBinary(path);
    ASSERT_TRUE(stated && text && bytes);
    auto text_status = (*text)->status();
    auto bytes_status = (*bytes)->status();
    ASSERT_TRUE(text_status && bytes_status);

    ASSERT_TRUE(stated->getUniqueID() == status->getUniqueID());
    ASSERT_TRUE(text_status->getUniqueID() == status->getUniqueID());
    ASSERT_TRUE(bytes_status->getUniqueID() == status->getUniqueID());
    ASSERT_EQ(stated->getSize(), 7u);
    ASSERT_EQ(text_status->getSize(), 7u);
    ASSERT_EQ(bytes_status->getSize(), 10u);
    ASSERT_TRUE(text_status->getLastModificationTime() == status->getLastModificationTime());
}

TEST_CASE(DirectoriesAndMissing) {
    TempDir tmp;
    tmp.touch("dir/a.h");

    auto dir = vfs::status(tmp.path("dir"));
    ASSERT_TRUE(dir.has_value());
    ASSERT_TRUE(dir->type() == llvm::sys::fs::file_type::directory_file);
    ASSERT_TRUE(dir->getUniqueID() == vfs::status(tmp.path("dir"))->getUniqueID());
    ASSERT_FALSE(dir->getUniqueID() == vfs::status(tmp.path("dir/a.h"))->getUniqueID());

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
    ASSERT_TRUE(vfs::status(a)->getUniqueID() == vfs::status(tmp.path("b.h"))->getUniqueID());
#ifndef _WIN32
    ASSERT_EQ(::symlink(a.c_str(), tmp.path("c.h").c_str()), 0);
    vfs::View view;
    auto linked = view.openFileForRead(tmp.path("c.h"));
    ASSERT_TRUE(bool(linked));
    ASSERT_TRUE((*linked)->status()->getUniqueID() == vfs::status(a)->getUniqueID());
    ASSERT_TRUE(view.status(tmp.path("c.h"))->getUniqueID() == vfs::status(a)->getUniqueID());
#endif
}

};  // TEST_SUITE(FileSystem)

}  // namespace

}  // namespace clice::testing
