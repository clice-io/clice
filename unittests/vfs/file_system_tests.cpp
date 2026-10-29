module;

#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.file_system;
import :vfs.path;

namespace clice::testing {

namespace {

ZEST_SUITE(FileSystem) {

ZEST_CASE(OpenedFilesKeepIdentity) {
    // Clang merges files by ID: a status by path and the status of the same
    // file opened, as text or as bytes, must agree on it.
    TempDir tmp;
    tmp.touch("a.h",
              "\xEF\xBB\xBF"
              "int x;\n");
    auto path = tmp.path("a.h");

    auto status = fs::status(path);
    ZASSERT(status);
    ZASSERT(status->size == 10u);

    vfs::View view;
    auto stated = view.status(path);
    auto text = view.openFileForRead(path);
    auto bytes = view.openFileForReadBinary(path);
    ZASSERT((stated && text && bytes));
    auto text_status = (*text)->status();
    auto bytes_status = (*bytes)->status();
    ZASSERT((text_status && bytes_status));

    auto id = vfs::to_llvm(*status, path).getUniqueID();
    ZASSERT(stated->getUniqueID() == id);
    ZASSERT(text_status->getUniqueID() == id);
    ZASSERT(bytes_status->getUniqueID() == id);
    ZASSERT(stated->getSize() == 7u);
    ZASSERT(text_status->getSize() == 7u);
    ZASSERT(bytes_status->getSize() == 10u);
    ZASSERT(text_status->getLastModificationTime() ==
            vfs::to_llvm(*status, path).getLastModificationTime());
}

ZEST_CASE(LinksShareIdentity) {
    TempDir tmp;
    tmp.touch("a.h", "int x;\n");
    auto a = tmp.path("a.h");
    ZASSERT(!fs::create_hard_link(a, tmp.path("b.h")));
    ZASSERT(vfs::Stamp::of(*fs::status(a)) == vfs::Stamp::of(*fs::status(tmp.path("b.h"))));
#ifndef _WIN32
    ZASSERT(::symlink(a.c_str(), tmp.path("c.h").c_str()) == 0);
    vfs::View view;
    auto linked = view.openFileForRead(tmp.path("c.h"));
    ZASSERT(bool(linked));
    auto id = vfs::to_llvm(*fs::status(a), a).getUniqueID();
    ZASSERT((*linked)->status()->getUniqueID() == id);
    ZASSERT(view.status(tmp.path("c.h"))->getUniqueID() == id);
#endif
}

ZEST_CASE(KeptTextFollowsEdits) {
    // A text read once is served to later compiles only while its file
    // still stats the same; an edit is read afresh.
    TempDir tmp;
    tmp.touch("a.h", "int x;\n");
    auto path = tmp.path("a.h");
    ZASSERT(set_file_mtime(path, file_mtime_ns(path) - 10'000'000'000));

    auto read = [&] {
        vfs::View view;
        auto file = view.openFileForRead(path);
        ZEXPECT(bool(file));
        auto status = (*file)->status();
        ZEXPECT(bool(status));
        ZEXPECT(status->getUniqueID() == vfs::to_llvm(*fs::status(path), path).getUniqueID());
        auto buffer = (*file)->getBuffer(path, -1, true, false);
        ZEXPECT(bool(buffer));
        ZEXPECT(status->getSize() == (*buffer)->getBufferSize());
        return std::move(*buffer);
    };
    auto first = read();
    auto second = read();
    ZASSERT(first->getBuffer() == "int x;\n");
    ZASSERT(first->getBufferStart() == second->getBufferStart());

    tmp.touch("a.h", "int y = 1;\n");
    ZASSERT(read()->getBuffer() == "int y = 1;\n");
}

ZEST_CASE(KeptMappingShared) {
    // A store artifact's mapping serves every later compile while the file
    // stats the same; any other file is mapped afresh.
    TempDir tmp;
    tmp.touch("a.pch", std::string(64 * 1024, 'x'));
    tmp.touch("b.pch", std::string(64 * 1024, 'x'));
    vfs::keep_mapped(tmp.path("a.pch"));

    auto map = [&](llvm::StringRef path) {
        vfs::View view;
        auto file = view.openFileForReadBinary(path);
        ZEXPECT(bool(file));
        auto buffer = (*file)->getBuffer(path, -1, false, false);
        ZEXPECT(bool(buffer));
        return std::move(*buffer);
    };
    auto a = tmp.path("a.pch");
    auto b = tmp.path("b.pch");
    ZASSERT(map(a)->getBufferStart() == map(a)->getBufferStart());
#ifdef _WIN32
    // The master and clang spell one PCH with different separators.
    auto slashed = a;
    ranges::replace(slashed, '\\', '/');
    ZASSERT(map(slashed)->getBufferStart() == map(a)->getBufferStart());
#endif
    ZASSERT(map(b)->getBufferStart() != map(b)->getBufferStart());
#ifndef _WIN32
    // Windows refuses to rewrite a mapped file; elsewhere a rewrite is seen.
    tmp.touch("a.pch", std::string(32 * 1024, 'y'));
    ZASSERT(map(a)->getBuffer() == std::string(32 * 1024, 'y'));
#endif
}

ZEST_CASE(BatchAgreesWithStatus) {
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
            auto direct = fs::status(path);
            ZASSERT((batched.has_value() && direct.has_value()));
            ZASSERT(vfs::Stamp::of(*batched) == vfs::Stamp::of(*direct));
            ZASSERT(batched->type == direct->type);
        }
    }
    auto sub = batch.status(tmp.path("dir/sub"));
    ZASSERT(sub);
    ZASSERT(sub->type == fs::FileType::Directory);
    // A directory's times in its parent's listing may lag behind its own:
    // only its identity is compared.
    auto direct = fs::status(tmp.path("dir/sub"));
    ZASSERT(sub->id == direct->id);
    auto missing = batch.status(tmp.path("dir/none.h"));
    ZASSERT(!missing.has_value());
    ZASSERT(missing.error() == std::errc::no_such_file_or_directory);
}

ZEST_CASE(BatchSeesLinkedEdits) {
    // NTFS updates a directory entry only for the link a write went
    // through: a directory holding hard links answers by name.
    TempDir tmp;
    for(int i = 0; i < 20; i += 1) {
        tmp.touch(std::format("dir/h{}.h", i), "x");
    }
    ZASSERT(!fs::create_hard_link(tmp.path("dir/h0.h"), tmp.path("dir/link.h")));
    tmp.touch("dir/h0.h", "a longer text");

    vfs::StatusBatch batch;
    ZASSERT(batch.status(tmp.path("dir/link.h"))->size == 13u);
    for(int i = 1; i < 20; i += 1) {
        ZASSERT(batch.status(tmp.path(std::format("dir/h{}.h", i))));
    }
    ZASSERT(batch.status(tmp.path("dir/link.h"))->size == 13u);
}

ZEST_CASE(StampSeesKeptTimes) {
    // A rewrite that keeps the size and puts the mtime back still moves
    // the change time.
    TempDir tmp;
    tmp.touch("a.h", "int x;\n");
    auto path = tmp.path("a.h");
    auto before = fs::status(path);
    ZASSERT(before);
    auto stamp = vfs::Stamp::of(*before);
    // Time is the feature: past the coarse clock inode times are taken from.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tmp.touch("a.h", "int y;\n");
    ZASSERT(set_file_mtime(path, stamp.mtime_ns));
    auto after = fs::status(path);
    ZASSERT(after);
    auto restamp = vfs::Stamp::of(*after);
    ZASSERT(restamp.size == stamp.size);
    ZASSERT(restamp.mtime_ns == stamp.mtime_ns);
    ZASSERT(restamp != stamp);
}

};  // ZEST_SUITE(FileSystem)

}  // namespace

}  // namespace clice::testing
