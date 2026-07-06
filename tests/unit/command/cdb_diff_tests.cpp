#include <algorithm>
#include <cstdint>

#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "command/argument_parser.h"
#include "command/command.h"
#include "support/filesystem.h"

namespace clice::testing {

namespace {

namespace ranges = std::ranges;

/// path_id that `cdb` assigns to a file under the temp root.
std::uint32_t id_of(CompilationDatabase& cdb, TempDir& tmp, llvm::StringRef rel) {
    return cdb.intern_path(path::join(tmp.root.str(), rel));
}

bool contains(llvm::ArrayRef<std::uint32_t> list, std::uint32_t id) {
    return ranges::find(list, id) != list.end();
}

/// Overwrite compile_commands.json under the temp root (without loading it).
void write_json(TempDir& tmp, llvm::ArrayRef<CDBEntry> entries) {
    tmp.touch("compile_commands.json", build_cdb_json(entries));
}

TEST_SUITE(ReloadDiff) {

TEST_CASE(AddedEntry) {
    TempDir tmp;
    CompilationDatabase cdb;
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
    auto diff = cdb.reload_and_diff(cdb_path);

    ASSERT_EQ(diff->added.size(), 1U);
    EXPECT_EQ(diff->added[0], id_of(cdb, tmp, "b.cpp"));
    EXPECT_TRUE(diff->removed.empty());
    EXPECT_TRUE(diff->changed.empty());
};

TEST_CASE(RemovedEntry) {
    TempDir tmp;
    CompilationDatabase cdb;
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
    auto diff = cdb.reload_and_diff(cdb_path);

    ASSERT_EQ(diff->removed.size(), 1U);
    EXPECT_EQ(diff->removed[0], id_of(cdb, tmp, "b.cpp"));
    EXPECT_TRUE(diff->added.empty());
    EXPECT_TRUE(diff->changed.empty());
};

TEST_CASE(ChangedFlag) {
    TempDir tmp;
    CompilationDatabase cdb;
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
    auto diff = cdb.reload_and_diff(cdb_path);

    ASSERT_EQ(diff->changed.size(), 1U);
    EXPECT_EQ(diff->changed[0], id_of(cdb, tmp, "a.cpp"));
    EXPECT_TRUE(diff->added.empty());
    EXPECT_TRUE(diff->removed.empty());
};

TEST_CASE(IdenticalReload) {
    TempDir tmp;
    CompilationDatabase cdb;
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-DFOO=1"}},
                   {tmp.root.str(), "b.cpp", {"-Wall"}  }
    });
    cdb.load(cdb_path);

    auto diff = cdb.reload_and_diff(cdb_path);
    EXPECT_TRUE(diff->empty());
};

TEST_CASE(ReorderNoDiff) {
    // A file's entries are compared as a set, so shuffling the JSON must not
    // register as a change — even when one file owns several entries.
    TempDir tmp;
    CompilationDatabase cdb;
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
    auto diff = cdb.reload_and_diff(cdb_path);

    EXPECT_TRUE(diff->empty());
};

TEST_CASE(CodegenChangeIgnored) {
    // Entry identity is the Frontend canonical hash, which drops codegen-only
    // flags. Swapping one codegen flag for another therefore yields no change.
    // (Note: -O* is NOT codegen-only here — it defines __OPTIMIZE__ and is
    // kept, so an -O change does count; see OptLevelIsSemantic.)
    TempDir tmp;
    CompilationDatabase cdb;
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
    auto diff = cdb.reload_and_diff(cdb_path);

    EXPECT_TRUE(diff->empty());
};

TEST_CASE(OptLevelIsSemantic) {
    // Anchors that -O* is semantic (defines __OPTIMIZE__), not codegen-only:
    // changing the optimization level must be reported as a change.
    TempDir tmp;
    CompilationDatabase cdb;
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
    auto diff = cdb.reload_and_diff(cdb_path);

    ASSERT_EQ(diff->changed.size(), 1U);
    EXPECT_EQ(diff->changed[0], id_of(cdb, tmp, "a.cpp"));
    EXPECT_TRUE(diff->added.empty());
    EXPECT_TRUE(diff->removed.empty());
};

TEST_CASE(MultiEntryOneChanged) {
    // A file with several entries appears in `changed` once, not per entry.
    TempDir tmp;
    CompilationDatabase cdb;
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
    auto diff = cdb.reload_and_diff(cdb_path);

    ASSERT_EQ(diff->changed.size(), 1U);
    EXPECT_EQ(diff->changed[0], id_of(cdb, tmp, "a.cpp"));
    EXPECT_TRUE(diff->added.empty());
    EXPECT_TRUE(diff->removed.empty());
};

TEST_CASE(FirstLoadAllAdded) {
    // Discovering a CDB for the first time: every file is `added`.
    TempDir tmp;
    CompilationDatabase cdb;
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {}},
                   {tmp.root.str(), "b.cpp", {}}
    });
    auto diff = cdb.reload_and_diff(cdb_path);

    ASSERT_EQ(diff->added.size(), 2U);
    EXPECT_TRUE(contains(diff->added, id_of(cdb, tmp, "a.cpp")));
    EXPECT_TRUE(contains(diff->added, id_of(cdb, tmp, "b.cpp")));
    EXPECT_TRUE(diff->removed.empty());
    EXPECT_TRUE(diff->changed.empty());
};

TEST_CASE(CorruptKeepsEntries) {
    // A half-written / corrupt CDB must leave the loaded entries intact and
    // signal failure so the caller retries instead of seeing "no change".
    TempDir tmp;
    CompilationDatabase cdb;
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {}}
    });
    cdb.load(cdb_path);
    ASSERT_TRUE(cdb.has_entry(path::join(tmp.root.str(), "a.cpp")));

    tmp.touch("compile_commands.json", "<<< corrupted compile_commands.json >>>");
    auto diff = cdb.reload_and_diff(cdb_path);

    ASSERT_FALSE(diff.has_value());
    EXPECT_TRUE(cdb.has_entry(path::join(tmp.root.str(), "a.cpp")));

    auto results = cdb.lookup(path::join(tmp.root.str(), "a.cpp"), {.inject_resource_dir = false});
    ASSERT_EQ(results.size(), 1U);
    EXPECT_TRUE(llvm::StringRef(print_argv(results.front().to_argv())).contains("-std=c++20"));
};

TEST_CASE(MissingFileFails) {
    // An unreadable file (deleted, or still locked by the generator) is a
    // failure, not an empty database: entries survive and the caller retries.
    TempDir tmp;
    CompilationDatabase cdb;
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {}}
    });
    cdb.load(cdb_path);
    fs::remove_all(cdb_path);

    auto diff = cdb.reload_and_diff(cdb_path);

    ASSERT_FALSE(diff.has_value());
    EXPECT_TRUE(cdb.has_entry(path::join(tmp.root.str(), "a.cpp")));
};

};  // TEST_SUITE(ReloadDiff)

}  // namespace

}  // namespace clice::testing
