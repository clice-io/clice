module clice;

import :project.memo;
import :project.project;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.file_table;

namespace clice::testing {
namespace {

ZEST_SUITE(Memo) {

ZEST_CASE(TraceHolds) {
    /// A result holds while the files it read hold; one changing forgets it.
    TempDir tmp;
    tmp.touch("a.h", "int a;\n");
    FileTable files;
    auto a = files.intern(Spelling::absolute(tmp.path("a.h")));
    Memo<Fid, int> memo;
    memo.commit(a, 1, {*current_dep(files, a)});
    ZASSERT(memo.lookup(files, a));
    ZEXPECT(memo.lookup(files, a)->result == 1);

    tmp.touch("a.h", "int b;\n");
    files.disk.end_turn();
    ZEXPECT(memo.lookup(files, a) == nullptr);
    ZEXPECT(memo.find(a) == nullptr);
};

ZEST_CASE(IncompleteTraceMisses) {
    /// A file the trace names at no version — the bytes read were never
    /// hashed — cannot prove the result holds.
    TempDir tmp;
    tmp.touch("a.h", "int a;\n");
    FileTable files;
    auto a = files.intern(Spelling::absolute(tmp.path("a.h")));
    Memo<Fid, int> memo;
    memo.commit(a, 1, {{.path_id = a}});
    ZEXPECT(memo.lookup(files, a) == nullptr);
};

};  // ZEST_SUITE(Memo)

}  // namespace
}  // namespace clice::testing
