module;

#include "modules/prelude.h"

module clice;

import :command.argument_parser;
import :command.command;
import :tests.unit.test.cdb_helper;
import :tests.unit.test.temp_dir;
import :tests.unit.test.test;
import :vfs.path;

namespace clice::testing {

namespace {

namespace ranges = std::ranges;

/// path_id that `cdb` assigns to a file under the temp root.
Fid id_of(CompilationDatabase& cdb, TempDir& tmp, llvm::StringRef rel) {
    return cdb.files().intern(Spelling::absolute(path::join(tmp.root.str(), rel)));
}

bool contains(llvm::ArrayRef<Fid> list, Fid id) {
    return ranges::find(list, id) != list.end();
}

/// Overwrite compile_commands.json under the temp root (without loading it).
void write_json(TempDir& tmp, llvm::ArrayRef<CDBEntry> entries) {
    tmp.touch("compile_commands.json", build_cdb_json(entries));
}

ZEST_SUITE(ReloadDiff) {

ZEST_CASE(AddedEntry) {
    TempDir tmp;
    FileTable file_table;
    CompilationDatabase cdb{file_table};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {}}
    });
    cdb.load(cdb_path);

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {}},
                   {tmp.root.str(), "b.cpp", {}}
    });
    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));

    ZASSERT(diff->added.size() == 1U);
    ZEXPECT(diff->added[0] == id_of(cdb, tmp, "b.cpp"));
    ZEXPECT(diff->removed.empty());
    ZEXPECT(diff->changed.empty());
};

ZEST_CASE(RemovedEntry) {
    TempDir tmp;
    FileTable file_table;
    CompilationDatabase cdb{file_table};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {}},
                   {tmp.root.str(), "b.cpp", {}}
    });
    cdb.load(cdb_path);

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {}}
    });
    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));

    ZASSERT(diff->removed.size() == 1U);
    ZEXPECT(diff->removed[0] == id_of(cdb, tmp, "b.cpp"));
    ZEXPECT(diff->added.empty());
    ZEXPECT(diff->changed.empty());
};

ZEST_CASE(ChangedFlag) {
    TempDir tmp;
    FileTable file_table;
    CompilationDatabase cdb{file_table};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-DFOO=1"}}
    });
    cdb.load(cdb_path);

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-DFOO=2"}}
    });
    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));

    ZASSERT(diff->changed.size() == 1U);
    ZEXPECT(diff->changed[0] == id_of(cdb, tmp, "a.cpp"));
    ZEXPECT(diff->added.empty());
    ZEXPECT(diff->removed.empty());
};

ZEST_CASE(IdenticalReload) {
    TempDir tmp;
    FileTable file_table;
    CompilationDatabase cdb{file_table};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-DFOO=1"}},
                   {tmp.root.str(), "b.cpp", {"-Wall"}  }
    });
    cdb.load(cdb_path);

    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));
    ZEXPECT(diff->empty());
};

ZEST_CASE(ReorderChangesSelection) {
    // Moving whole files around the JSON changes nothing, but swapping one
    // file's entries does: the first entry is its default selection.
    TempDir tmp;
    FileTable file_table;
    CompilationDatabase cdb{file_table};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-DA=1"}},
                   {tmp.root.str(), "a.cpp", {"-DB=1"}},
                   {tmp.root.str(), "b.cpp", {}       }
    });
    cdb.load(cdb_path);

    write_json(tmp,
               {
                   {tmp.root.str(), "b.cpp", {}       },
                   {tmp.root.str(), "a.cpp", {"-DB=1"}},
                   {tmp.root.str(), "a.cpp", {"-DA=1"}}
    });
    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));

    ZASSERT(diff);
    ZEXPECT(diff->added.empty());
    ZEXPECT(diff->removed.empty());
    ZEXPECT(diff->changed ==
            llvm::SmallVector<Fid>{file_table.intern(Spelling::absolute(tmp.path("a.cpp")))});
};

ZEST_CASE(CodegenChangeIgnored) {
    // Entry identity is the Frontend canonical hash, which drops codegen-only
    // flags. Swapping one codegen flag for another therefore yields no change.
    // (Note: -O* is NOT codegen-only here — it defines __OPTIMIZE__ and is
    // kept, so an -O change does count; see OptLevelIsSemantic.)
    TempDir tmp;
    FileTable file_table;
    CompilationDatabase cdb{file_table};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-fPIC", "-g"}}
    });
    cdb.load(cdb_path);

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-fno-omit-frame-pointer", "-flto"}}
    });
    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));

    ZEXPECT(diff->empty());
};

ZEST_CASE(OptLevelIsSemantic) {
    // Anchors that -O* is semantic (defines __OPTIMIZE__), not codegen-only:
    // changing the optimization level must be reported as a change.
    TempDir tmp;
    FileTable file_table;
    CompilationDatabase cdb{file_table};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-O2"}}
    });
    cdb.load(cdb_path);

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-O3"}}
    });
    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));

    ZASSERT(diff->changed.size() == 1U);
    ZEXPECT(diff->changed[0] == id_of(cdb, tmp, "a.cpp"));
    ZEXPECT(diff->added.empty());
    ZEXPECT(diff->removed.empty());
};

ZEST_CASE(MultiEntryOneChanged) {
    // A file with several entries appears in `changed` once, not per entry.
    TempDir tmp;
    FileTable file_table;
    CompilationDatabase cdb{file_table};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-DA=1"}},
                   {tmp.root.str(), "a.cpp", {"-DB=1"}}
    });
    cdb.load(cdb_path);

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-DA=2"}},
                   {tmp.root.str(), "a.cpp", {"-DB=1"}}
    });
    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));

    ZASSERT(diff->changed.size() == 1U);
    ZEXPECT(diff->changed[0] == id_of(cdb, tmp, "a.cpp"));
    ZEXPECT(diff->added.empty());
    ZEXPECT(diff->removed.empty());
};

ZEST_CASE(FirstLoadAllAdded) {
    // Discovering a CDB for the first time: every file is `added`.
    TempDir tmp;
    FileTable file_table;
    CompilationDatabase cdb{file_table};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {}},
                   {tmp.root.str(), "b.cpp", {}}
    });
    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));

    ZASSERT(diff->added.size() == 2U);
    ZEXPECT(contains(diff->added, id_of(cdb, tmp, "a.cpp")));
    ZEXPECT(contains(diff->added, id_of(cdb, tmp, "b.cpp")));
    ZEXPECT(diff->removed.empty());
    ZEXPECT(diff->changed.empty());
};

ZEST_CASE(CorruptKeepsEntries) {
    // A half-written / corrupt CDB must leave the loaded entries intact and
    // signal failure so the caller retries instead of seeing "no change".
    TempDir tmp;
    FileTable file_table;
    CompilationDatabase cdb{file_table};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {}}
    });
    cdb.load(cdb_path);
    ZASSERT(cdb.has_entry(path::join(tmp.root.str(), "a.cpp")));

    tmp.touch("compile_commands.json", "<<< corrupted compile_commands.json >>>");
    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));

    ZASSERT(!diff.has_value());
    ZEXPECT(cdb.has_entry(path::join(tmp.root.str(), "a.cpp")));

    auto file = path::join(tmp.root.str(), "a.cpp");
    auto candidates = cdb.candidate_entries(file);
    ZASSERT(candidates.size() == 1U);
    ZEXPECT(llvm::StringRef(print_argv(cdb.render_full(candidates.front().config)))
                .contains("-std=c++20"));
};

ZEST_CASE(MissingFileFails) {
    // An unreadable file (deleted, or still locked by the generator) is a
    // failure, not an empty database: entries survive and the caller retries.
    TempDir tmp;
    FileTable file_table;
    CompilationDatabase cdb{file_table};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {}}
    });
    cdb.load(cdb_path);
    vfs::remove_all(cdb_path);

    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));

    ZASSERT(!diff.has_value());
    ZEXPECT(cdb.has_entry(path::join(tmp.root.str(), "a.cpp")));
};

};  // ZEST_SUITE(ReloadDiff)

}  // namespace

}  // namespace clice::testing
