#ifndef _WIN32
#include <unistd.h>
#endif

#include "test/temp_dir.h"
#include "test/test.h"
#include "support/filesystem.h"
#include "syntax/include_resolver.h"
#include "vfs/file_table.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/xxhash.h"

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

TEST_CASE(HardlinkFirstBindReads) {
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

#ifndef _WIN32
// The four tests below pin identity-based defenses (rename-over rebinds,
// hardlink merging, live-identity stamp gates) that require file-stable
// UniqueIDs — POSIX-only; see fs::stable_file_ids and FileTable::entity_key.
TEST_CASE(RenameSaveRebinds) {
    // An editor-style save (write tmp, rename over) replaces the inode.
    // The forged stat makes the old pair match by (size, mtime): only the
    // rebind on the changed UniqueID keeps it from vouching for the new
    // bytes.
    TempDir tmp;
    tmp.touch("f.h", "int v1();\n");
    auto f = tmp.path("f.h");
    age(f);

    FileTable pool;
    auto fid = pool.intern(f);
    auto first = pool.read(fid);
    ASSERT_TRUE(first.has_value());

    tmp.touch("f.h.tmp", "int v2();\n");
    ASSERT_TRUE(bool(fs::rename(tmp.path("f.h.tmp"), f)));
    EXPECT_TRUE(set_file_mtime(f, first->mtime_ns));

    auto status = stat_of(f);
    auto uid = status.getUniqueID();
    ASSERT_EQ(status.getSize(), first->size);
    ASSERT_EQ(fs::mtime_ns(status), first->mtime_ns);
    ASSERT_FALSE(pool.cached_hash(fid,
                                  status.getSize(),
                                  fs::mtime_ns(status),
                                  uid.getDevice(),
                                  uid.getFile())
                     .has_value());

    auto reread = pool.read(fid);
    ASSERT_TRUE(reread.has_value());
    ASSERT_NE(reread->hash, first->hash);
    ASSERT_TRUE(
        pool.cached_hash(fid, reread->size, reread->mtime_ns, reread->uid_device, reread->uid_file)
            .has_value());
}

TEST_CASE(FastPathChecksIdentity) {
    // The file's stat fast path must not survive a rename-over that
    // forges the same size and mtime: the inode changed, and the session
    // knows this fid's identity.
    TempDir tmp;
    tmp.touch("f.h", "int v1();\n");
    auto f = tmp.path("f.h");
    age(f);

    FileTable pool;
    auto fid = pool.intern(f);
    auto read = pool.read(fid);
    ASSERT_TRUE(read.has_value());
    auto vid = pool.intern_version(fid, read->hash);
    ASSERT_TRUE(pool.cached_hash(fid, read->size, read->mtime_ns, read->uid_device, read->uid_file)
                    .has_value());

    tmp.touch("f.h.tmp", "int v2();\n");
    ASSERT_TRUE(bool(fs::rename(tmp.path("f.h.tmp"), f)));
    EXPECT_TRUE(set_file_mtime(f, read->mtime_ns));

    auto wave = pool.wave();
    ASSERT_TRUE(pool.check_version(vid) == FileTable::Verdict::Stale);
}

TEST_CASE(SymlinkShownAsSpelled) {
    // A file is its resolved path; results name it the way the user does:
    // the open document's spelling, else the workspace root's.
    TempDir tmp;
    tmp.touch("real/a.h", "");
    ASSERT_EQ(::symlink(tmp.path("real").c_str(), tmp.path("link").c_str()), 0);
    auto real = CanonicalPath(tmp.path("real/a.h"));
    auto link = tmp.path("link/a.h");

    FileTable pool;
    auto fid = pool.intern(link);
    ASSERT_EQ(pool.intern(real), fid);
    ASSERT_EQ(pool.resolve(fid), real);
    ASSERT_EQ(pool.display(fid), llvm::StringRef(real));

    pool.spell_root(tmp.path("link"));
    ASSERT_EQ(pool.display(fid), link);

    pool.show_as(fid, real);
    ASSERT_EQ(pool.display(fid), llvm::StringRef(real));
    pool.unshow(fid);
    ASSERT_EQ(pool.display(fid), link);
}

TEST_CASE(PairNeedsLiveIdentity) {
    // The shared pair answers only through the identity it was earned
    // under: a stat carrying another UniqueID (a same-stat replace) must
    // read, even when size and mtime match.
    TempDir tmp;
    tmp.touch("f.h", "int v1();\n");
    auto f = tmp.path("f.h");
    age(f);

    FileTable pool;
    auto fid = pool.intern(f);
    auto read = pool.read(fid);
    ASSERT_TRUE(read.has_value());
    ASSERT_FALSE(
        pool.cached_hash(fid, read->size, read->mtime_ns, read->uid_device + 1, read->uid_file + 1)
            .has_value());
}
#endif

TEST_CASE(FreshReadNotVouched) {
    // A check that reads a just-written file gets the right verdict but
    // must not keep the stat as the fast path: on a coarse-mtime
    // filesystem a same-tick write could later forge it.
    TempDir tmp;
    tmp.touch("f.h", "int fresh();\n");
    auto f = tmp.path("f.h");

    FileTable pool;
    auto fid = pool.intern(f);
    auto read = pool.read(fid);
    ASSERT_TRUE(read.has_value());
    auto vid = pool.intern_version(fid, read->hash);

    auto wave = pool.wave();
    ASSERT_TRUE(pool.check_version(vid) == FileTable::Verdict::Fresh);
    ASSERT_FALSE(pool.cached_hash(fid, read->size, read->mtime_ns, read->uid_device, read->uid_file)
                     .has_value());
}

TEST_CASE(ReadDropsBom) {
    // A file saved with a UTF-8 byte order mark reads as the text an editor
    // sends: both sides of every buffer-versus-disk comparison agree.
    TempDir tmp;
    tmp.touch("bom.h", "\xEF\xBB\xBFint x;\n");
    auto observed = read_file_observed(tmp.path("bom.h").c_str());
    ASSERT_TRUE(observed.has_value());
    ASSERT_EQ(observed->content->getBuffer(), "int x;\n");
    ASSERT_EQ(observed->obs.hash, llvm::xxh3_64bits("int x;\n"));
}

TEST_CASE(CompileFSDropsBom) {
    // The compile's file system serves the same text, its stat agreeing on
    // the size as clang checks.
    TempDir tmp;
    tmp.touch("bom.h", "\xEF\xBB\xBFint x;\n");
    auto path = tmp.path("bom.h");
    ThreadSafeFS vfs;
    auto status = vfs.status(path);
    ASSERT_TRUE(bool(status));
    ASSERT_EQ(status->getSize(), 7u);
    auto file = vfs.openFileForRead(path);
    ASSERT_TRUE(bool(file));
    auto buffer = (*file)->getBuffer(path, -1, true, false);
    ASSERT_TRUE(bool(buffer));
    ASSERT_EQ((*buffer)->getBuffer(), "int x;\n");
    ASSERT_EQ((*file)->status()->getSize(), 7u);
}

TEST_CASE(ListingSeesNewFile) {
    // An external generator dropping a header into a cached directory
    // produces no event; the directory's own mtime is the anchor that
    // makes the next operation re-list it.
    TempDir tmp;
    tmp.touch("inc/a.h", "");
    auto dir = tmp.path("inc");
    age(dir);
    auto aged = file_mtime_ns(dir);

    FileTable pool;
    DirListingCache first_op;
    first_op.shared = &pool;
    auto* entries = resolve_dir(dir, first_op);
    ASSERT_TRUE(entries != nullptr);
    ASSERT_TRUE(entries->contains("a.h"));
    ASSERT_FALSE(entries->contains("b.h"));

    tmp.touch("inc/b.h", "");
    // A second age() rewinds relative to now and can land on the first
    // listing's exact stamp within one coarse mtime tick, revalidating the
    // cached listing; a fixed offset keeps the stamps distinct while
    // staying outside the guard window.
    ASSERT_TRUE(set_file_mtime(dir, aged + 1'000'000'000));

    DirListingCache second_op;
    second_op.shared = &pool;
    auto* refreshed = resolve_dir(dir, second_op);
    ASSERT_TRUE(refreshed != nullptr);
    ASSERT_TRUE(refreshed->contains("b.h"));
}

TEST_CASE(WarmListingReused) {
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
    EXPECT_EQ(pool.resolve(pool.intern(R"(a\b)")).str(), R"(a\b)");
}
#endif

};  // TEST_SUITE(FileTable)

}  // namespace

}  // namespace clice::testing
