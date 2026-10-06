module;

#include "modules/prelude.h"
#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :tests.unit.test.temp_dir;
import :tests.unit.test.test;
import :vfs.file_system;

namespace clice::testing {

namespace {

/// A link to a directory: a symlink, or on Windows a junction, which needs
/// no privilege to make.
bool link_directory(const std::string& target, const std::string& link) {
#ifdef _WIN32
    auto cmd = llvm::sys::findProgramByName("cmd");
    std::optional<llvm::StringRef> quiet[] = {std::nullopt,
                                              llvm::StringRef(""),
                                              llvm::StringRef("")};
    return cmd && llvm::sys::ExecuteAndWait(*cmd,
                                            {"cmd", "/c", "mklink", "/J", link, target},
                                            {},
                                            quiet) == 0;
#else
    return ::symlink(target.c_str(), link.c_str()) == 0;
#endif
}

ZEST_SUITE(FileSystem) {

ZEST_CASE(OpenedFilesKeepIdentity) {
    // Clang merges files by ID: a status by path and the status of the same
    // file opened, as text or as bytes, must agree on it.
    TempDir tmp;
    tmp.touch("a.h",
              "\xEF\xBB\xBF"
              "int x;\n");
    auto path = tmp.path("a.h");

    auto status = vfs::status(path);
    ZASSERT(status);
    ZASSERT(status->stamp.size == 10u);

    vfs::View view;
    auto stated = view.status(path);
    auto text = view.openFileForRead(path);
    auto bytes = view.openFileForReadBinary(path);
    ZASSERT((stated && text && bytes));
    auto text_status = (*text)->status();
    auto bytes_status = (*bytes)->status();
    ZASSERT((text_status && bytes_status));

    auto id = status->to_llvm(path).getUniqueID();
    ZASSERT(stated->getUniqueID() == id);
    ZASSERT(text_status->getUniqueID() == id);
    ZASSERT(bytes_status->getUniqueID() == id);
    ZASSERT(stated->getSize() == 7u);
    ZASSERT(text_status->getSize() == 7u);
    ZASSERT(bytes_status->getSize() == 10u);
    ZASSERT(text_status->getLastModificationTime() ==
            status->to_llvm(path).getLastModificationTime());
}

ZEST_CASE(DirectoriesAndMissing) {
    TempDir tmp;
    tmp.touch("dir/a.h");

    auto dir = vfs::status(tmp.path("dir"));
    ZASSERT(dir);
    ZASSERT(dir->type == llvm::sys::fs::file_type::directory_file);
    ZASSERT(dir->stamp.file == vfs::status(tmp.path("dir"))->stamp.file);
    ZASSERT(dir->stamp.file != vfs::status(tmp.path("dir/a.h"))->stamp.file);

    auto missing = vfs::status(tmp.path("dir/b.h"));
    ZASSERT(!missing.has_value());
    ZASSERT(missing.error() == std::errc::no_such_file_or_directory);
    auto no_parent = vfs::status(tmp.path("none/b.h"));
    ZASSERT(!no_parent.has_value());
    ZASSERT(no_parent.error() == std::errc::no_such_file_or_directory);
}

ZEST_CASE(LinksShareIdentity) {
    TempDir tmp;
    tmp.touch("a.h", "int x;\n");
    auto a = tmp.path("a.h");
    ZASSERT(!bool(llvm::sys::fs::create_hard_link(a, tmp.path("b.h"))));
    ZASSERT(vfs::status(a)->stamp == vfs::status(tmp.path("b.h"))->stamp);
#ifndef _WIN32
    ZASSERT(::symlink(a.c_str(), tmp.path("c.h").c_str()) == 0);
    vfs::View view;
    auto linked = view.openFileForRead(tmp.path("c.h"));
    ZASSERT(bool(linked));
    auto id = vfs::status(a)->to_llvm(a).getUniqueID();
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
        ZEXPECT(status->getUniqueID() == vfs::status(path)->to_llvm(path).getUniqueID());
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
    std::ranges::replace(slashed, '\\', '/');
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
            auto direct = vfs::status(path);
            ZASSERT((batched.has_value() && direct.has_value()));
            ZASSERT(batched->stamp == direct->stamp);
            ZASSERT(batched->type == direct->type);
        }
    }
    auto sub = batch.status(tmp.path("dir/sub"));
    ZASSERT(sub);
    ZASSERT(sub->type == llvm::sys::fs::file_type::directory_file);
    // A directory's times in its parent's listing may lag behind its own:
    // only its identity is compared.
    auto direct = vfs::status(tmp.path("dir/sub"));
    ZASSERT(sub->stamp.device == direct->stamp.device);
    ZASSERT(sub->stamp.file == direct->stamp.file);
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
    ZASSERT(!bool(llvm::sys::fs::create_hard_link(tmp.path("dir/h0.h"), tmp.path("dir/link.h"))));
    tmp.touch("dir/h0.h", "a longer text");

    vfs::StatusBatch batch;
    ZASSERT(batch.status(tmp.path("dir/link.h"))->stamp.size == 13u);
    for(int i = 1; i < 20; i += 1) {
        ZASSERT(batch.status(tmp.path(std::format("dir/h{}.h", i))));
    }
    ZASSERT(batch.status(tmp.path("dir/link.h"))->stamp.size == 13u);
}

ZEST_CASE(StampSeesKeptTimes) {
    // A rewrite that keeps the size and puts the mtime back still moves
    // the change time.
    TempDir tmp;
    tmp.touch("a.h", "int x;\n");
    auto path = tmp.path("a.h");
    auto before = vfs::status(path);
    ZASSERT(before);
    // Past the coarse clock inode times are taken from.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tmp.touch("a.h", "int y;\n");
    ZASSERT(set_file_mtime(path, before->stamp.mtime_ns));
    auto after = vfs::status(path);
    ZASSERT(after);
    ZASSERT(after->stamp.size == before->stamp.size);
    ZASSERT(after->stamp.mtime_ns == before->stamp.mtime_ns);
    ZASSERT(after->stamp != before->stamp);
}

ZEST_CASE(WalkPrunesAndSkipsLinks) {
    TempDir tmp;
    tmp.touch("src/a.cpp");
    tmp.touch("src/deep/b.cpp");
    tmp.touch("skip/c.cpp");
    tmp.touch("outside/d.cpp");
    ZASSERT(link_directory(tmp.path("outside"), tmp.path("src/link")));

    std::vector<std::string> seen;
    vfs::walk(tmp.path("src"), [&](const vfs::Entry& entry) {
        seen.push_back(llvm::sys::path::filename(entry.path).str());
        if(seen.back() == "link") {
            ZEXPECT(entry.type == llvm::sys::fs::file_type::symlink_file);
        }
        return true;
    });
    std::ranges::sort(seen);
    ZASSERT(seen == (std::vector<std::string>{"a.cpp", "b.cpp", "deep", "link"}));

    seen.clear();
    vfs::walk(tmp.root, [&](const vfs::Entry& entry) {
        auto name = llvm::sys::path::filename(entry.path);
        seen.push_back(name.str());
        return name != "skip" && name != "src";
    });
    std::ranges::sort(seen);
    ZASSERT(seen == (std::vector<std::string>{"d.cpp", "outside", "skip", "src"}));
}

ZEST_CASE(LinksSeenAsLinks) {
    TempDir tmp;
    tmp.mkdir("target");
    ZASSERT(link_directory(tmp.path("target"), tmp.path("link")));
    ZASSERT(vfs::is_symlink(tmp.path("link")));
    ZASSERT(!vfs::is_symlink(tmp.path("target")));
    ZASSERT(vfs::exists(tmp.path("link")));
    ZASSERT(!static_cast<bool>(vfs::remove(tmp.path("target"))));
    ZASSERT(!vfs::exists(tmp.path("link")));
    ZASSERT(vfs::is_symlink(tmp.path("link")));
}

ZEST_CASE(RemoveAllKeepsLinkTargets) {
    TempDir tmp;
    tmp.touch("tree/a/b.h");
    tmp.touch("tree/c.h");
    tmp.touch("outside/kept.h");
    ZASSERT(link_directory(tmp.path("outside"), tmp.path("tree/link")));
    ZASSERT(link_directory(tmp.path("outside"), tmp.path("root-link")));
    ZASSERT(!static_cast<bool>(vfs::remove_all(tmp.path("root-link"))));
    ZASSERT(!vfs::is_symlink(tmp.path("root-link")));
    ZASSERT(!static_cast<bool>(vfs::remove_all(tmp.path("tree"))));
    ZASSERT(!vfs::exists(tmp.path("tree")));
    ZASSERT(vfs::exists(tmp.path("outside/kept.h")));
    ZASSERT(!static_cast<bool>(vfs::remove_all(tmp.path("tree"))));
}

ZEST_CASE(RemoveMappedFile) {
    // Workers keep PCHs mapped while the store replaces them.
    TempDir tmp;
    // Not a whole number of pages: a null-terminated read maps only then.
    tmp.touch("blob.pch", std::string((1 << 20) + 1, 'x'));
    auto path = tmp.path("blob.pch");
    auto mapped = vfs::read(path, vfs::Read::Mapped);
    ZASSERT(mapped);
    ZASSERT((*mapped)->getBufferKind() == llvm::MemoryBuffer::MemoryBuffer_MMap);
    ZASSERT(!static_cast<bool>(vfs::remove(path)));
    ZASSERT(!vfs::exists(path));
    ZASSERT((*mapped)->getBufferSize() == (1u << 20) + 1);
    tmp.touch("blob.pch", "new");
    ZASSERT(read_file(path).value_or("") == "new");
    ZASSERT(!static_cast<bool>(vfs::remove(path)));
    ZASSERT(!static_cast<bool>(vfs::remove(path)));
}

ZEST_CASE(AtomicWriteReplaces) {
    TempDir tmp;
    auto path = tmp.path("state.json");
    ZASSERT(!static_cast<bool>(vfs::write_atomic(path, "old")));
    ZASSERT(!static_cast<bool>(vfs::write_atomic(path, "new")));
    ZASSERT(read_file(path).value_or("") == "new");
    auto entries = vfs::read_dir(tmp.root);
    ZASSERT(entries);
    ZASSERT(entries->size() == 1u);
    ZASSERT(entries->front().type == llvm::sys::fs::file_type::regular_file);
    ZASSERT(static_cast<bool>(vfs::write_atomic(tmp.path("none/state.json"), "x")));
}

};  // ZEST_SUITE(FileSystem)

}  // namespace

}  // namespace clice::testing
