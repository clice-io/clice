module;

#include "modules/prelude.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/ConvertUTF.h"
#else
#include <unistd.h>
#endif

module clice;

import :tests.unit.test.temp_dir;
import :tests.unit.test.test;
import :vfs.file_table;
import :vfs.path;

namespace clice::testing {

namespace {

/// Rewind a file's (or directory's) mtime out of the guard window so its
/// observations count as reliable, the way real project files predate a
/// server start.
void age(llvm::StringRef path) {
    ZEXPECT(set_file_mtime(path, file_mtime_ns(path) - 10'000'000'000));
}

vfs::Stamp stamp_of(llvm::StringRef path) {
    auto status = vfs::status(path);
    ZEXPECT(status);
    return status ? status->stamp : vfs::Stamp{};
}

ZEST_SUITE(FileTable) {

ZEST_CASE(HardlinksReadApart) {
    // Two spellings hardlinked to one file are two files: a read through
    // one never vouches for the other.
    TempDir tmp;
    tmp.touch("a.h", "int shared();\n");
    auto a = tmp.path("a.h");
    auto b = tmp.path("b.h");
    ZASSERT(!bool(llvm::sys::fs::create_hard_link(a, b)));
    age(a);

    FileTable pool;
    auto a_id = pool.intern(Spelling::absolute(a));
    auto b_id = pool.intern(Spelling::absolute(b));
    ZASSERT(a_id != b_id);
    ZASSERT(pool.read(a_id));

    auto stamp = stamp_of(b);
    ZASSERT(pool.cached_hash(a_id, stamp));
    ZASSERT(!pool.cached_hash(b_id, stamp).has_value());
    ZASSERT(pool.read(b_id));
    ZASSERT(pool.cached_hash(b_id, stamp));
}

ZEST_CASE(RenameSaveReads) {
    // An editor-style save (write tmp, rename over) replaces the file.
    // The forged size and mtime match the old pair: the file ID and change
    // time in the stamp keep it from vouching for the new bytes.
    TempDir tmp;
    tmp.touch("f.h", "int v1();\n");
    auto f = tmp.path("f.h");
    age(f);

    FileTable pool;
    auto fid = pool.intern(Spelling::absolute(f));
    auto first = pool.read(fid);
    ZASSERT(first);

    tmp.touch("f.h.tmp", "int v2();\n");
    ZASSERT(!vfs::rename(tmp.path("f.h.tmp"), f));
    ZEXPECT(set_file_mtime(f, first->stamp.mtime_ns));

    auto stamp = stamp_of(f);
    ZASSERT(stamp.size == first->stamp.size);
    ZASSERT(stamp.mtime_ns == first->stamp.mtime_ns);
    ZASSERT(!pool.cached_hash(fid, stamp).has_value());

    auto reread = pool.read(fid);
    ZASSERT(reread);
    ZASSERT(reread->hash != first->hash);
    ZASSERT(pool.cached_hash(fid, reread->stamp));
}

ZEST_CASE(FastPathChecksIdentity) {
    // The file's stat fast path must not survive a rename-over that
    // forges the same size and mtime: the inode changed, and the session
    // knows this fid's identity.
    TempDir tmp;
    tmp.touch("f.h", "int v1();\n");
    auto f = tmp.path("f.h");
    age(f);

    FileTable pool;
    auto fid = pool.intern(Spelling::absolute(f));
    auto read = pool.read(fid);
    ZASSERT(read);
    auto vid = pool.intern_version(fid, read->hash);
    ZASSERT(pool.cached_hash(fid, read->stamp));

    tmp.touch("f.h.tmp", "int v2();\n");
    ZASSERT(!vfs::rename(tmp.path("f.h.tmp"), f));
    ZEXPECT(set_file_mtime(f, read->stamp.mtime_ns));

    ZASSERT(pool.check_version(vid) == vfs::DiskState::Verdict::Stale);
}

ZEST_CASE(PairNeedsSameFile) {
    // A pair answers only the stamp it was read under: a status carrying
    // another file ID (a same-stat replace) must read, even when size and
    // times match.
    TempDir tmp;
    tmp.touch("f.h", "int v1();\n");
    auto f = tmp.path("f.h");
    age(f);

    FileTable pool;
    auto fid = pool.intern(Spelling::absolute(f));
    auto read = pool.read(fid);
    ZASSERT(read);
    auto other = read->stamp;
    other.device += 1;
    other.file += 1;
    ZASSERT(!pool.cached_hash(fid, other).has_value());
}

#ifndef _WIN32
ZEST_CASE(SymlinkShownAsSpelled) {
    // A file is its resolved path; results name it the way the user does:
    // the open document's spelling, else the workspace root's — never the
    // spelling an include lookup reached it by.
    TempDir tmp;
    tmp.touch("real/a.h", "");
    ZASSERT(::symlink(tmp.path("real").c_str(), tmp.path("link").c_str()) == 0);
    auto real = CanonicalPath(Spelling::absolute(tmp.path("real/a.h")));
    auto link = tmp.path("link/a.h");

    FileTable pool;
    auto fid = pool.intern_spelled(Spelling::absolute(link));
    ZASSERT(pool.intern(real) == fid);
    ZASSERT(pool.resolve(fid) == real);
    ZASSERT(pool.display(fid) == llvm::StringRef(real));

    pool.spell_root(Spelling::absolute(tmp.path("link")));
    ZASSERT(pool.display(fid) == link);

    pool.show_as(fid, real);
    ZASSERT(pool.display(fid) == llvm::StringRef(real));
    pool.unshow(fid);
    ZASSERT(pool.display(fid) == link);

    pool.unspell_root(Spelling::absolute(tmp.path("link")));
    ZASSERT(pool.display(fid) == llvm::StringRef(real));
}

ZEST_CASE(RootClimbPastSymlink) {
    // A folder spelled with `..` shows without the climb while the folded
    // text names the same directory, under the symlink it went through;
    // a climb out of a symlinked directory lands elsewhere than its text
    // says, and the folder shows by its identity.
    TempDir tmp;
    tmp.touch("real/proj/inc/h.h", "");
    tmp.mkdir("real/proj/build");
    tmp.mkdir("other");
    ZASSERT(::symlink(tmp.path("real").c_str(), tmp.path("link").c_str()) == 0);
    ZASSERT(::symlink(tmp.path("real/proj/build").c_str(), tmp.path("other/build").c_str()) == 0);
    auto header = Spelling::absolute(tmp.path("real/proj/inc/h.h"));

    FileTable through;
    through.spell_root(Spelling::absolute(tmp.path("link/proj/build/..")));
    ZASSERT(through.display(through.intern(header)) == tmp.path("link/proj/inc/h.h"));

    FileTable out;
    out.spell_root(Spelling::absolute(tmp.path("other/build/..")));
    auto fid = out.intern(header);
    ZASSERT(out.display(fid) == out.resolve(fid).str());
}
#endif

ZEST_CASE(FreshReadNotVouched) {
    // A check that reads a just-written file gets the right verdict but
    // must not keep the stat as the fast path: on a coarse-mtime
    // filesystem a same-tick write could later forge it.
    TempDir tmp;
    tmp.touch("f.h", "int fresh();\n");
    auto f = tmp.path("f.h");

    FileTable pool;
    auto fid = pool.intern(Spelling::absolute(f));
    auto read = pool.read(fid);
    ZASSERT(read);
    auto vid = pool.intern_version(fid, read->hash);

    ZASSERT(pool.check_version(vid) == vfs::DiskState::Verdict::Fresh);
    ZASSERT(!pool.cached_hash(fid, read->stamp).has_value());
}

ZEST_CASE(ReadDropsBom) {
    // A file saved with a UTF-8 byte order mark reads as the text an editor
    // sends: both sides of every buffer-versus-disk comparison agree.
    TempDir tmp;
    tmp.touch("bom.h", "\xEF\xBB\xBFint x;\n");
    auto observed = vfs::read_observed(tmp.path("bom.h"));
    ZASSERT(observed);
    ZASSERT(observed->content->getBuffer() == "int x;\n");
    ZASSERT(observed->obs.hash == llvm::xxh3_64bits("int x;\n"));
}

ZEST_CASE(CompileFSDropsBom) {
    // The compile's file system serves the same text, its stat agreeing on
    // the size as clang checks.
    TempDir tmp;
    tmp.touch("bom.h", "\xEF\xBB\xBFint x;\n");
    auto path = tmp.path("bom.h");
    vfs::View view;
    auto status = view.status(path);
    ZASSERT(bool(status));
    ZASSERT(status->getSize() == 7u);
    auto file = view.openFileForRead(path);
    ZASSERT(bool(file));
    auto buffer = (*file)->getBuffer(path, -1, true, false);
    ZASSERT(bool(buffer));
    ZASSERT((*buffer)->getBuffer() == "int x;\n");
    ZASSERT((*file)->status()->getSize() == 7u);
}

ZEST_CASE(BinaryReadKeepsBom) {
    // `#embed` data, PCH and PCM files are bytes, served as they are.
    TempDir tmp;
    tmp.touch("data.bin",
              "\xEF\xBB\xBF"
              "AB");
    auto path = tmp.path("data.bin");
    vfs::View view;
    auto file = view.openFileForReadBinary(path);
    ZASSERT(bool(file));
    ZASSERT((*file)->status()->getSize() == 5u);
    auto buffer = (*file)->getBuffer(path, -1, true, false);
    ZASSERT(bool(buffer));
    ZASSERT((*buffer)->getBuffer() ==
            "\xEF\xBB\xBF"
            "AB");
}

ZEST_CASE(ReadModesServeBom) {
    TempDir tmp;
    tmp.touch("bom.txt",
              "\xEF\xBB\xBF"
              "AB");
    auto path = tmp.path("bom.txt");
    ZASSERT((*vfs::read(path))->getBuffer() == "AB");
    ZASSERT((*vfs::read(path, vfs::Read::Bytes))->getBuffer() ==
            "\xEF\xBB\xBF"
            "AB");
    ZASSERT((*vfs::read(path, vfs::Read::Mapped))->getBuffer() ==
            "\xEF\xBB\xBF"
            "AB");
}

ZEST_CASE(ListingKnowsDirectories) {
    // Include completion tells directories from headers by the listing.
    TempDir tmp;
    tmp.touch("inc/a.h", "");
    tmp.touch("inc/sub/b.h", "");
#ifndef _WIN32
    ZASSERT(::symlink(tmp.path("inc/sub").c_str(), tmp.path("inc/link").c_str()) == 0);
#endif
    auto listing = vfs::list(tmp.path("inc"));
    ZASSERT(!listing->entries.lookup("a.h"));
    ZASSERT(listing->entries.lookup("sub"));
#ifndef _WIN32
    ZASSERT(listing->entries.lookup("link"));
#endif
}

ZEST_CASE(ListingSeesNewFile) {
    // An external generator dropping a header into a cached directory
    // produces no event; the directory's own mtime is the anchor that
    // makes the next operation re-list it.
    TempDir tmp;
    tmp.touch("inc/a.h", "");
    auto dir = tmp.path("inc");
    age(dir);
    auto aged = file_mtime_ns(dir);

    vfs::DirCache cache;
    vfs::Scope first_op(cache);
    auto& entries = first_op.list(dir);
    ZASSERT(entries.contains("a.h"));
    ZASSERT(!entries.contains("b.h"));

    tmp.touch("inc/b.h", "");
    // A second age() rewinds relative to now and can land on the first
    // listing's exact stamp within one coarse mtime tick, revalidating the
    // cached listing; a fixed offset keeps the stamps distinct while
    // staying outside the guard window.
    ZASSERT(set_file_mtime(dir, aged + 1'000'000'000));

    vfs::Scope second_op(cache);
    ZASSERT(second_op.list(dir).contains("b.h"));
    // The first operation keeps the listing it validated.
    ZASSERT(!entries.contains("b.h"));
}

ZEST_CASE(WarmListingReused) {
    TempDir tmp;
    tmp.touch("inc/a.h", "");
    auto dir = tmp.path("inc");
    age(dir);

    vfs::DirCache cache;
    {
        vfs::Scope op(cache);
        op.list(dir);
    }
    ZASSERT(cache.listings.contains(dir));
    ZASSERT(cache.listings.find(dir)->second->mtime_ns != 0);

    // The next operation validates by one stat and reuses the listing.
    vfs::Scope op(cache);
    ZASSERT(op.list(dir).contains("a.h"));
    ZASSERT(op.stats.listed == 0u);
    ZASSERT(op.stats.reused == 1u);
}

ZEST_CASE(LookupSpellingNotShown) {
    // A Meson-style build reaches a header through `build/../include`: the
    // lookup keeps that spelling, the user is shown the file itself.
    TempDir tmp;
    tmp.touch("include/lib.h", "");
    tmp.mkdir("build");
    auto spelled = Spelling::absolute(tmp.path("build/../include/lib.h"));

    FileTable pool;
    auto fid = pool.intern_spelled(spelled);
    ZASSERT(pool.spelling(fid).str() == spelled.str());
    ZASSERT(pool.display(fid) == pool.resolve(fid).str());
}

ZEST_CASE(RootDotDotFolded) {
    // `--workspace ..` run in the build directory names its parent.
    TempDir tmp;
    tmp.touch("proj/inc/h.h", "");
    tmp.mkdir("proj/build");

    FileTable pool;
    pool.spell_root(Spelling::absolute(tmp.path("proj/build/..")));
    auto fid = pool.intern(Spelling::absolute(tmp.path("proj/inc/h.h")));
    ZASSERT(pool.display(fid) == pool.resolve(fid).str());
}

ZEST_CASE(CanonicalSpelling) {
    // The rewrite itself is platform-independent and testable anywhere;
    // only its application is Windows-gated.
    auto canon = [](std::string s) {
        path::make_canonical(llvm::MutableArrayRef(s.data(), s.size()));
        return s;
    };
    ZEXPECT(canon(R"(D:\ws\x.h)") == "d:/ws/x.h");
    ZEXPECT(canon("d:/ws/x.h") == "d:/ws/x.h");
    ZEXPECT(canon("/usr/X.h") == "/usr/X.h");

    ZEXPECT(path::needs_canonical(R"(a\b)"));
    ZEXPECT(path::needs_canonical("C:/x.h"));
    ZEXPECT(!path::needs_canonical("c:/x.h"));
    ZEXPECT(!path::needs_canonical("/usr/x.h"));
}

ZEST_CASE(PortableNames) {
    // Under the workspace a path is named relative to it, anywhere else it
    // keeps its own name; a name reads back in whichever checkout holds it.
    llvm::SmallString<64> storage;
    ZEXPECT(path::portable("/w/src/a.cpp", "/w", storage) == "${workspace}/src/a.cpp");
    ZEXPECT(path::portable("/w", "/w", storage) == "${workspace}");
    ZEXPECT(path::portable("/a.cpp", "/", storage) == "${workspace}/a.cpp");
    ZEXPECT(path::portable("/wx/a.cpp", "/w", storage) == "/wx/a.cpp");
    ZEXPECT(path::portable("/w/a.cpp", "", storage) == "/w/a.cpp");

    ZEXPECT(path::local("${workspace}/src/a.cpp", "/moved", storage) == "/moved/src/a.cpp");
    ZEXPECT(path::local("${workspace}", "/moved", storage) == "/moved");
    ZEXPECT(path::local("${workspace}/a.cpp", "/", storage) == "/a.cpp");
    ZEXPECT(path::local("/wx/a.cpp", "/moved", storage) == "/wx/a.cpp");
}

#ifdef _WIN32
ZEST_CASE(WindowsSpellingsCollapse) {
    // VS Code sends lowercase drive URIs while the CDB and clang report
    // uppercase; on Windows every spelling of one file interns to one ID
    // and resolves to the client-facing form, or every CDB lookup misses
    // and compiles fall back to guessed commands.
    FileTable pool;
    ZEXPECT(pool.intern(Spelling::absolute("c:/a/b.h")) ==
            pool.intern(Spelling::absolute(R"(C:\a\b.h)")));
    ZEXPECT(pool.resolve(pool.intern(Spelling::absolute("C:/a/b.h"))).str() == "c:/a/b.h");
    ZEXPECT(pool.find(Spelling::absolute(R"(c:\a\b.h)")) ==
            pool.find(Spelling::absolute("C:/a/b.h")));
}

ZEST_CASE(DriveRootSpelled) {
    // A subst drive opened at its root: the folder's spelling ends in its
    // separator, and the rest of a path joins it without a second one.
    TempDir tmp;
    tmp.touch("inc/h.h", "");
    auto taken = ::GetLogicalDrives();
    auto letter = 'Z';
    while(letter > 'D' && (taken & (1u << (letter - 'A')))) {
        letter -= 1;
    }
    ZASSERT(letter > 'D');
    std::wstring device{static_cast<wchar_t>(letter), L':'};
    std::wstring target;
    ZASSERT(llvm::ConvertUTF8toWide(tmp.root.str(), target));
    ZASSERT(::DefineDosDeviceW(0, device.c_str(), target.c_str()));
    auto undefine = llvm::make_scope_exit(
        [&] { ::DefineDosDeviceW(DDD_REMOVE_DEFINITION, device.c_str(), nullptr); });
    std::string drive{static_cast<char>(llvm::toLower(letter)), ':', '/'};

    FileTable pool;
    pool.spell_root(Spelling::absolute(drive));
    auto fid = pool.intern(Spelling::absolute(tmp.path("inc/h.h")));
    ZASSERT(pool.display(fid) == drive + "inc/h.h");
}

ZEST_CASE(WindowsCaseVariantsMerge) {
    // The worker names a file by the OS's final name for it, in on-disk
    // case; a spelling in another case interning to a second fid splits
    // the file's dependencies and index rows between the two.
    TempDir tmp;
    tmp.touch("Real/File.h", "");
    FileTable pool;
    auto fid = pool.intern(Spelling::absolute(tmp.path("real/file.H")));
    ZEXPECT(pool.intern(Spelling::absolute(tmp.path("Real/File.h"))) == fid);
    ZEXPECT(llvm::StringRef(pool.resolve(fid)).ends_with("/Real/File.h"));
}

ZEST_CASE(LongPathCaseMerges) {
    // Past MAX_PATH the OS opens a path only with the `\\?\` prefix;
    // without it the identity falls back to the spelling's own case.
    TempDir tmp;
    std::string deep;
    for(int i = 0; i < 6; i += 1) {
        deep += std::string(50, static_cast<char>('a' + i)) + "/";
    }
    tmp.touch(deep + "Real/File.h", "");
    ZASSERT(tmp.path(deep + "Real/File.h").size() > MAX_PATH);
    FileTable pool;
    auto fid = pool.intern(Spelling::absolute(tmp.path(deep + "real/file.H")));
    ZEXPECT(pool.intern(Spelling::absolute(tmp.path(deep + "Real/File.h"))) == fid);
    ZEXPECT(llvm::StringRef(pool.resolve(fid)).ends_with("/Real/File.h"));
}
#else
ZEST_CASE(PosixBytesPreserved) {
    // '\' and "C:" are ordinary filename characters on POSIX; identity is
    // the raw bytes and the Windows rewrite must not touch them.
    FileTable pool;
    ZEXPECT(pool.intern(Spelling::absolute(R"(/w/a\b)")) !=
            pool.intern(Spelling::absolute("/w/a/b")));
    ZEXPECT(pool.intern(Spelling::absolute("/w/C:/x.h")) !=
            pool.intern(Spelling::absolute("/w/c:/x.h")));
    ZEXPECT(pool.intern(Spelling::absolute("/c/x.h")) != pool.intern(Spelling::absolute("/C/x.h")));
    ZEXPECT(pool.resolve(pool.intern(Spelling::absolute(R"(/w/a\b)"))).str() == R"(/w/a\b)");
}
#endif

};  // ZEST_SUITE(FileTable)

}  // namespace

}  // namespace clice::testing
