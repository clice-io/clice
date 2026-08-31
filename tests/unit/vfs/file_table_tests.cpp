#include "test/temp_dir.h"
#include "test/test.h"
#include "support/filesystem.h"
#include "syntax/include_resolver.h"
#include "vfs/file_table.h"

#include "llvm/Support/FileSystem.h"

namespace clice::testing {

namespace {

/// Rewind a file's (or directory's) mtime out of the guard window so its
/// observations count as reliable, the way real project files predate a
/// server start.
void age(llvm::StringRef path) {
    EXPECT_TRUE(set_file_mtime(path, file_mtime_ns(path) - 10'000'000'000));
}

llvm::sys::fs::file_status stat_of(llvm::StringRef path) {
    llvm::sys::fs::file_status status;
    EXPECT_FALSE(bool(llvm::sys::fs::status(path, status)));
    return status;
}

TEST_SUITE(FileTable) {

TEST_CASE(FirstBindToExistingEntityReads) {
    // Two spellings hardlinked to one inode: the second spelling's first
    // sight of the (already known) entity must not inherit the pair — a
    // recycled inode can hand an unrelated new file an equal-looking stat.
    // After one read through the second spelling, the twins share.
    TempDir tmp;
    tmp.touch("a.h", "int shared();\n");
    auto a = tmp.path("a.h");
    auto b = tmp.path("b.h");
    ASSERT_FALSE(bool(llvm::sys::fs::create_hard_link(a, b)));
    age(a);

    FileTable pool;
    auto a_id = pool.intern(a);
    auto b_id = pool.intern(b);
    auto read = pool.read(a_id);
    ASSERT_TRUE(read.has_value());

    auto status = stat_of(b);
    auto uid = status.getUniqueID();
    ASSERT_FALSE(pool.cached_hash(b_id,
                                  status.getSize(),
                                  fs::mtime_ns(status),
                                  uid.getDevice(),
                                  uid.getFile())
                     .has_value());

    ASSERT_TRUE(pool.read(b_id).has_value());
    auto earned = pool.cached_hash(b_id,
                                   status.getSize(),
                                   fs::mtime_ns(status),
                                   uid.getDevice(),
                                   uid.getFile());
    ASSERT_TRUE(earned.has_value());
    ASSERT_EQ(*earned, read->hash);

    // The earned binding also serves the first spelling still.
    auto a_status = stat_of(a);
    ASSERT_TRUE(pool.cached_hash(a_id,
                                 a_status.getSize(),
                                 fs::mtime_ns(a_status),
                                 a_status.getUniqueID().getDevice(),
                                 a_status.getUniqueID().getFile())
                    .has_value());
}

TEST_CASE(RenameSaveRebinds) {
    // An editor-style save (write tmp, rename over) replaces the inode:
    // the spelling rebinds to the fresh entity and nothing serves the old
    // pair for the new bytes.
    TempDir tmp;
    tmp.touch("f.h", "int v1();\n");
    auto f = tmp.path("f.h");
    age(f);

    FileTable pool;
    auto fid = pool.intern(f);
    ASSERT_TRUE(pool.read(fid).has_value());

    tmp.touch("f.h.tmp", "int v2();\n");
    ASSERT_TRUE(bool(fs::rename(tmp.path("f.h.tmp"), f)));
    age(f);

    auto status = stat_of(f);
    auto uid = status.getUniqueID();
    ASSERT_FALSE(pool.cached_hash(fid,
                                  status.getSize(),
                                  fs::mtime_ns(status),
                                  uid.getDevice(),
                                  uid.getFile())
                     .has_value());

    auto reread = pool.read(fid);
    ASSERT_TRUE(reread.has_value());
    ASSERT_TRUE(
        pool.cached_hash(fid, reread->size, reread->mtime_ns, reread->uid_device, reread->uid_file)
            .has_value());
}

TEST_CASE(DirListingSeesNewFile) {
    // An external generator dropping a header into a cached directory
    // produces no event; the directory's own mtime is the anchor that
    // makes the next operation re-list it.
    TempDir tmp;
    tmp.touch("inc/a.h", "");
    auto dir = tmp.path("inc");
    age(dir);

    FileTable pool;
    DirListingCache first_op;
    first_op.shared = &pool;
    auto* entries = resolve_dir(dir, first_op);
    ASSERT_TRUE(entries != nullptr);
    ASSERT_TRUE(entries->contains("a.h"));
    ASSERT_FALSE(entries->contains("b.h"));

    tmp.touch("inc/b.h", "");
    age(dir);

    DirListingCache second_op;
    second_op.shared = &pool;
    auto* refreshed = resolve_dir(dir, second_op);
    ASSERT_TRUE(refreshed != nullptr);
    ASSERT_TRUE(refreshed->contains("b.h"));
}

TEST_CASE(DirListingWarmWithoutChange) {
    TempDir tmp;
    tmp.touch("inc/a.h", "");
    auto dir = tmp.path("inc");
    age(dir);

    FileTable pool;
    {
        DirListingCache op;
        op.shared = &pool;
        resolve_dir(dir, op);
    }
    ASSERT_TRUE(pool.dir_listings.contains(dir));
    ASSERT_TRUE(pool.dir_listings.find(dir)->second.mtime_ns != 0);

    // The next operation validates by one stat and reuses the listing.
    StatCounters counters;
    DirListingCache op;
    op.shared = &pool;
    auto* entries = resolve_dir(dir, op, &counters);
    ASSERT_TRUE(entries->contains("a.h"));
    ASSERT_EQ(counters.dir_listings, 0u);
    ASSERT_EQ(counters.dir_hits, 1u);
}

TEST_CASE(CanonicalSpelling) {
    // The rewrite itself is platform-independent and testable anywhere;
    // only its application is Windows-gated.
    auto canon = [](std::string s) {
        path::make_canonical(llvm::MutableArrayRef(s.data(), s.size()));
        return s;
    };
    EXPECT_EQ(canon(R"(D:\ws\x.h)"), "d:/ws/x.h");
    EXPECT_EQ(canon("d:/ws/x.h"), "d:/ws/x.h");
    EXPECT_EQ(canon("/usr/X.h"), "/usr/X.h");

    EXPECT_TRUE(path::needs_canonical(R"(a\b)"));
    EXPECT_TRUE(path::needs_canonical("C:/x.h"));
    EXPECT_FALSE(path::needs_canonical("c:/x.h"));
    EXPECT_FALSE(path::needs_canonical("/usr/x.h"));
}

#ifdef _WIN32
TEST_CASE(WindowsSpellingsCollapse) {
    // VS Code sends lowercase drive URIs while the CDB and clang report
    // uppercase; on Windows every spelling of one file interns to one ID
    // and resolves to the client-facing form, or every CDB lookup misses
    // and compiles fall back to guessed commands.
    FileTable pool;
    EXPECT_EQ(pool.intern("c:/a/b.h"), pool.intern(R"(C:\a\b.h)"));
    EXPECT_EQ(pool.resolve(pool.intern("C:/a/b.h")), "c:/a/b.h");
    EXPECT_EQ(pool.find(R"(c:\a\b.h)"), pool.find("C:/a/b.h"));
}
#else
TEST_CASE(PosixBytesPreserved) {
    // '\' and "C:" are ordinary filename characters on POSIX; identity is
    // the raw bytes and the Windows rewrite must not touch them.
    FileTable pool;
    EXPECT_NE(pool.intern(R"(a\b)"), pool.intern("a/b"));
    EXPECT_NE(pool.intern("C:/x.h"), pool.intern("c:/x.h"));
    EXPECT_NE(pool.intern("/c/x.h"), pool.intern("/C/x.h"));
    EXPECT_EQ(pool.resolve(pool.intern(R"(a\b)")), R"(a\b)");
}
#endif

};  // TEST_SUITE(FileTable)

}  // namespace

}  // namespace clice::testing
