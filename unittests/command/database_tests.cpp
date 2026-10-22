module;

#include "modules/prelude.h"
#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :command.database;
import :command.table;
import :support.shell;
import :unittests.utils.cdb_helper;
import :unittests.utils.cdb_layouts;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.file_system;
import :vfs.path;

namespace clice::testing {

namespace {

using namespace std::literals;

ZEST_SUITE(Database) {

#ifndef _WIN32
ZEST_CASE(SymlinkedSourceSpelling) {
    /// A database entry naming a symlinked source compiles under that name,
    /// as the build does; its identity stays the file it points to.
    TempDir tmp;
    tmp.touch("real/main.cpp", "int main() {}\n");
    ZASSERT(::symlink(tmp.path("real/main.cpp").c_str(), tmp.path("main.cpp").c_str()) == 0);
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    ZASSERT(database.load(tmp.path("compile_commands.json")));
    auto argv = render_entry(database, tmp.path("real/main.cpp"));
    ZASSERT(!argv.empty());
    ZEXPECT(llvm::StringRef(argv.back()) == tmp.path("main.cpp"));

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("real/main.cpp"), {}}
    }));
    ZASSERT(database.load(tmp.path("compile_commands.json")));
    argv = render_entry(database, tmp.path("real/main.cpp"));
    ZASSERT(!argv.empty());
    ZEXPECT(llvm::StringRef(argv.back()) == tmp.path("real/main.cpp"));
};
#endif

ZEST_CASE(ResponseFilesRecorded) {
    /// A load records the response files its commands name among its
    /// inputs, the ones it could not read included, so the tracker can
    /// watch them all.
    TempDir tmp;
    tmp.touch("flags.rsp", "-DFROM_RSP=1\n");
    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"),  {"@flags.rsp", "@missing.rsp"}},
                  {tmp.root, tmp.path("other.cpp"), {"@flags.rsp"}                }
    }));
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    auto id = database.add_source(Spelling::absolute(tmp.path("compile_commands.json")));
    ZASSERT(database.load_source(id));
    auto recorded = database.inputs(id);
    ZASSERT(recorded.size() == 3u);
    ZEXPECT(file_table.resolve(recorded[0].file) ==
            CanonicalPath(Spelling::absolute(tmp.path("compile_commands.json"))));
    ZEXPECT(recorded[0].hash);
    ZEXPECT(file_table.resolve(recorded[1].file) ==
            CanonicalPath(Spelling::absolute(path::join(tmp.root, "flags.rsp"))));
    ZEXPECT(recorded[1].hash);
    ZEXPECT(file_table.resolve(recorded[2].file) ==
            CanonicalPath(Spelling::absolute(path::join(tmp.root, "missing.rsp"))));
    ZEXPECT(!recorded[2].hash.has_value());
    ZEXPECT(database.present(id));

    tmp.touch("compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {"@flags.rsp"}}
    }));
    ZASSERT(database.load_source(id));
    ZEXPECT(database.inputs(id).size() == 2u);
};

ZEST_CASE(OptionsFilesRecorded) {
    /// An nvcc command's options files are load inputs like response
    /// files: editing one changes the commands as much as the database.
    TempDir tmp;
    tmp.touch("opts.rsp", "-DFROM_OPTIONS=1\n");
    tmp.touch("more.rsp", "-DFROM_MORE=1\n");
    tmp.touch(
        "compile_commands.json",
        std::format(
            R"([{{"directory": "{}", "file": "kern.cu", "command": "nvcc -optf opts.rsp,missing.rsp --options-file=more.rsp -c kern.cu"}}])",
            json_escape(tmp.root)));
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    auto id = database.add_source(Spelling::absolute(tmp.path("compile_commands.json")));
    ZASSERT(database.load_source(id));
    auto recorded = database.inputs(id);
    ZASSERT(recorded.size() == 4u);
    auto names = [&](std::size_t i, llvm::StringRef name) {
        return file_table.resolve(recorded[i].file) ==
               CanonicalPath(Spelling::absolute(path::join(tmp.root, name)));
    };
    ZEXPECT(names(1, "opts.rsp"));
    ZEXPECT(recorded[1].hash);
    ZEXPECT(names(2, "missing.rsp"));
    ZEXPECT(!recorded[2].hash.has_value());
    ZEXPECT(names(3, "more.rsp"));
    ZEXPECT(recorded[3].hash);
};

ZEST_CASE(MultiCommand) {
    /// A file can have multiple compilation commands (e.g. different configs).
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "main.cpp", "clang++ -std=c++17 main.cpp"sv);
    database.add_command("/fake", "main.cpp", "clang++ -std=c++20 main.cpp"sv);
    database.add_command("/fake", "other.cpp", "clang++ -std=c++23 other.cpp"sv);

    auto candidates = database.candidate_entries(fake("main.cpp"));
    ZASSERT(candidates.size() == 2U);

    /// Both commands are present, in file order.
    bool has_17 = false, has_20 = false;
    for(auto& entry: candidates) {
        auto argv = print_argv(commands.render_full(entry.command));
        if(llvm::StringRef(argv).contains("-std=c++17"))
            has_17 = true;
        if(llvm::StringRef(argv).contains("-std=c++20"))
            has_20 = true;
    }
    ZEXPECT(has_17);
    ZEXPECT(has_20);

    ZASSERT(database.candidate_entries(fake("other.cpp")).size() == 1U);
};

ZEST_CASE(CandidateOrderIsFileOrder) {
    /// A file's entries keep the order their source lists them in:
    /// generators emit configurations in a fixed order, so the first entry
    /// is the same configuration for every file.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "main.cpp", "clang++ -std=c++20 main.cpp"sv);
    database.add_command("/fake", "main.cpp", "clang++ -std=c++17 main.cpp"sv);
    database.add_command("/fake", "other.cpp", "clang++ -std=c++20 other.cpp"sv);
    database.add_command("/fake", "other.cpp", "clang++ -std=c++17 other.cpp"sv);

    ZEXPECT(llvm::StringRef(print_argv(render_entry(database, fake("main.cpp"))))
                .contains("-std=c++20"));
    ZEXPECT(llvm::StringRef(print_argv(render_entry(database, fake("other.cpp"))))
                .contains("-std=c++20"));
    auto candidates = database.candidate_entries(fake("main.cpp"));
    ZASSERT(candidates.size() == 2U);
    ZEXPECT(candidates[0].ordinal == 0U);
    ZEXPECT(candidates[1].ordinal == 1U);
};

ZEST_CASE(MultipleSources) {
    /// Two sources load side by side; a file present in both keeps one
    /// candidate per source, and reloading one source leaves the other's
    /// entries alone.
    TempDir tmp;
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    auto dir = json_escape(tmp.root);
    tmp.touch("a/compile_commands.json", R"([{"directory": ")" + dir + R"(", "file": "shared.cpp",
                  "arguments": ["clang++", "-DFROM_A", "shared.cpp"]},
                 {"directory": ")" + dir + R"(", "file": "only_a.cpp",
                  "arguments": ["clang++", "only_a.cpp"]}])");
    tmp.touch("b/compile_commands.json", R"([{"directory": ")" + dir + R"(", "file": "shared.cpp",
                  "arguments": ["clang++", "-DFROM_B", "shared.cpp"]}])");

    auto a = database.add_source(Spelling::absolute(tmp.path("a")));
    auto b = database.add_source(Spelling::absolute(tmp.path("b/compile_commands.json")));
    ZEXPECT(database.add_source(Spelling::absolute(tmp.path("a/compile_commands.json"))) == a);
    ZASSERT(database.load_source(a).value() == 2U);
    ZASSERT(database.load_source(b).value() == 1U);
    ZEXPECT(database.loaded(a));

    auto shared = database.candidate_entries(tmp.path("shared.cpp"));
    ZASSERT(shared.size() == 2U);
    ZEXPECT(shared[0].source == a);
    ZEXPECT(shared[1].source == b);
    ZEXPECT(database.entries().size() == 3U);

    auto diff = database.unload_source(a);
    ZEXPECT(!database.loaded(a));
    ZEXPECT(diff.removed.size() == 1U);
    ZEXPECT(diff.changed.size() == 1U);
    ZEXPECT(database.candidate_entries(tmp.path("shared.cpp")).size() == 1U);
    ZEXPECT(database.candidate_entries(tmp.path("only_a.cpp")).empty());
    ZEXPECT(database.load_source(a).value() == 2U);
    ZEXPECT(database.entries().size() == 3U);
};

ZEST_CASE(LoadMixedFormats) {
    /// "arguments" array and "command" string can coexist in the same CDB.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    auto count = write_cdb(tmp, database, R"([{"directory": ")" + dir + R"(", "file": "a.cpp",
          "arguments": ["clang++", "-std=c++20", "a.cpp"]},
         {"directory": ")" + dir + R"(", "file": "b.cpp",
          "command": "clang++ -std=c++23 b.cpp"}])");

    ZASSERT(count == 2U);
    ZEXPECT(llvm::StringRef(print_argv(render_entry(database, tmp.path("a.cpp"))))
                .contains("-std=c++20"));
    ZEXPECT(llvm::StringRef(print_argv(render_entry(database, tmp.path("b.cpp"))))
                .contains("-std=c++23"));
};

ZEST_CASE(RelativeDirectoryAnchored) {
    /// A relative CDB `directory` anchors to the CDB file's own location,
    /// both for resolving the entry's file and as the command's directory.
    TempDir tmp;
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    tmp.touch("compile_commands.json", R"([
        {"directory": "build", "file": "main.cpp",
         "arguments": ["clang++", "-std=c++20", "main.cpp"]}
    ])");
    ZASSERT(database.load(tmp.path("compile_commands.json")).value_or(0) == 1U);

    auto file = path::join(tmp.root, "build", "main.cpp");
    auto candidates = database.candidate_entries(file);
    ZASSERT(candidates.size() == 1U);
    ZEXPECT(llvm::StringRef(commands.get(candidates.front().command).directory) ==
            Spelling::absolute(path::join(tmp.root, "build")).str());
};

ZEST_CASE(RelativeLoadPathAnchored) {
    /// A relative CDB path from the command line anchors at the working
    /// directory before load(), so no entry carries a relative identity.
    TempDir tmp;
    tmp.touch("compile_commands.json", R"([
        {"directory": "build", "file": "main.cpp",
         "arguments": ["clang++", "-std=c++20", "main.cpp"]}
    ])");

    llvm::SmallString<256> saved_cwd;
    ZASSERT(!bool(llvm::sys::fs::current_path(saved_cwd)));
    ZASSERT(!bool(llvm::sys::fs::set_current_path(tmp.root)));
    auto restore = llvm::make_scope_exit([&] { llvm::sys::fs::set_current_path(saved_cwd); });

    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    ZASSERT(database.load(Spelling("compile_commands.json", Spelling::cwd())).value_or(0) == 1U);

    auto candidates = database.candidate_entries(path::join(tmp.root, "build", "main.cpp"));
    ZASSERT(candidates.size() == 1U);
    ZEXPECT(llvm::StringRef(commands.get(candidates.front().command).directory) ==
            Spelling::absolute(path::join(tmp.root, "build")).str());
};

ZEST_CASE(LoadErrorRecovery) {
    /// Bad entries should be skipped; good entries still load.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    auto count = write_cdb(tmp,
                           database,
                           R"([{"file": "no_dir.cpp",
          "arguments": ["clang++", "no_dir.cpp"]},
         {"directory": ")" + dir +
                               R"(",
          "arguments": ["clang++", "no_file.cpp"]},
         {"directory": ")" + dir +
                               R"(", "file": "no_args.cpp"},
         {"directory": ")" + dir +
                               R"(", "file": "good.cpp",
          "arguments": ["clang++", "-std=c++20", "good.cpp"]},
         42,
         {"directory": ")" + dir +
                               R"(", "file": "also_good.cpp",
          "command": "clang++ -Wall also_good.cpp"}])");

    ZASSERT(count == 2U);
    ZEXPECT(llvm::StringRef(print_argv(render_entry(database, tmp.path("good.cpp"))))
                .contains("-std=c++20"));
    ZEXPECT(llvm::StringRef(print_argv(render_entry(database, tmp.path("also_good.cpp"))))
                .contains("-Wall"));
};

ZEST_CASE(LoadCudaHeader) {
    /// .cuh entries are C-family despite clang's extension table; non-C
    /// entries some build systems emit are skipped.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    auto count = write_cdb(tmp, database, R"([{"directory": ")" + dir + R"(", "file": "kernels.cuh",
          "command": "nvcc -c kernels.cuh -o kernels.o"},
         {"directory": ")" + dir + R"(", "file": "app.rc",
          "command": "rc /fo app.res app.rc"}])");

    ZASSERT(count == 1U);
    ZEXPECT(database.has_entry(tmp.path("kernels.cuh")));
};

ZEST_CASE(LoadForcedLanguage) {
    /// An extension clang does not know is C-family when the command's -x
    /// says so.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    auto count = write_cdb(tmp, database, R"([{"directory": ")" + dir + R"(", "file": "a.ixx",
          "command": "clang++ -std=c++20 -x c++-module -c a.ixx"},
         {"directory": ")" + dir + R"(", "file": "b.ixx",
          "command": "clang++ -std=c++20 -c b.ixx"}])");

    ZASSERT(count == 1U);
    ZEXPECT(database.has_entry(tmp.path("a.ixx")));
};

ZEST_CASE(LoadEmptyCommand) {
    /// Whitespace-only or empty "command" should not crash.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    auto count = write_cdb(tmp,
                           database,
                           R"([{"directory": ")" + dir +
                               R"(", "file": "empty.cpp", "command": ""},
         {"directory": ")" + dir +
                               R"(", "file": "spaces.cpp", "command": "   "},
         {"directory": ")" + dir +
                               R"(", "file": "ok.cpp",
          "command": "clang++ -std=c++20 ok.cpp"}])");

    ZASSERT(count == 1U);
    ZEXPECT(llvm::StringRef(print_argv(render_entry(database, tmp.path("ok.cpp"))))
                .contains("-std=c++20"));
};

ZEST_CASE(LoadReload) {
    /// Loading the same source again replaces all of its entries.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};

    auto file_a = tmp.path("a.cpp");
    auto file_b = tmp.path("b.cpp");
    auto cdb_path = tmp.path("compile_commands.json");

    tmp.touch("compile_commands.json", R"([{"directory": ")" + dir + R"(", "file": "a.cpp",
          "arguments": ["clang++", "-std=c++17", "a.cpp"]}])");
    ZASSERT(database.load(cdb_path).value_or(0) == 1U);
    ZEXPECT(llvm::StringRef(print_argv(render_entry(database, file_a))).contains("-std=c++17"));

    tmp.touch("compile_commands.json", R"([{"directory": ")" + dir + R"(", "file": "b.cpp",
          "arguments": ["clang++", "-std=c++23", "b.cpp"]}])");
    auto count = database.load(cdb_path).value_or(0);
    ZASSERT(count == 1U);
    ZEXPECT(database.source_count() == 1U);

    ZEXPECT(database.candidate_entries(file_a).empty());
    ZEXPECT(llvm::StringRef(print_argv(render_entry(database, file_b))).contains("-std=c++23"));
};

ZEST_CASE(LoadCommandQuoting) {
    /// "command" string with spaces in paths and quoted defines.
    TempDir tmp;
    auto dir = json_escape(tmp.root);
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    auto count = write_cdb(tmp, database, R"([{"directory": ")" + dir + R"(", "file": "main.cpp",
          "command": "clang++ -std=c++20 \"-DMSG=hello world\" -I\"/path with spaces\" main.cpp"}])");

    ZASSERT(count == 1U);
    auto argv = print_argv(render_entry(database, tmp.path("main.cpp")));
    ZEXPECT(llvm::StringRef(argv).contains("hello world"));
    ZEXPECT(llvm::StringRef(argv).contains("with spaces"));
};

ZEST_CASE(LoadRelativePath) {
    /// load() resolves relative file paths against the directory.
    TempDir tmp;
    auto project = tmp.path("project/build");
    auto other = tmp.path("other/build");
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    auto count = write_cdb(tmp,
                           database,
                           R"([{"directory": ")" + json_escape(project) +
                               R"(", "file": "src/main.cpp",
          "arguments": ["clang++", "-std=c++20", "src/main.cpp"]},
         {"directory": ")" + json_escape(other) +
                               R"(", "file": "src/main.cpp",
          "arguments": ["clang++", "-std=c++17", "src/main.cpp"]}])");

    ZASSERT(count == 2U);

    ZEXPECT(
        llvm::StringRef(print_argv(render_entry(database, path::join(project, "src", "main.cpp"))))
            .contains("-std=c++20"));
    ZEXPECT(
        llvm::StringRef(print_argv(render_entry(database, path::join(other, "src", "main.cpp"))))
            .contains("-std=c++17"));

    /// The same relative spelling anchored elsewhere is a different path —
    /// no entry.
    ZEXPECT(database.candidate_entries(tmp.path("src/main.cpp")).empty());
};

ZEST_CASE(LoadDotSegments) {
    /// Entry paths intern without . and .. segments, so lookups against
    /// clang-reported (realpath'd) spellings match.
    TempDir tmp;
    auto build = tmp.path("project/build");
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    auto count = write_cdb(tmp,
                           database,
                           R"([{"directory": ")" + json_escape(build) +
                               R"(", "file": "../src/./main.cpp",
          "arguments": ["clang++", "-std=c++20", "../src/./main.cpp"]}])");

    ZASSERT(count == 1U);
    ZEXPECT(database.has_entry(tmp.path("project/src/main.cpp")));
};

ZEST_CASE(FixtureLayouts) {
    /// The layouts' databases go through load() like a real project's; each
    /// pins the one property it exists for. Macros are asserted by name: the
    /// driver render spells -D with a separate value.
    TempDir dir;
    for(auto& layout: cdb_layouts()) {
        write_layout(dir, layout.name);
    }
    auto layouts = std::string(dir.root);
    auto load_layout = [&](CompilationDatabase& database,
                           llvm::StringRef layout,
                           llvm::StringRef cdb = "compile_commands.json") {
        return database.load(path::join(layouts, layout, cdb)).value_or(0);
    };
    auto source = [&](llvm::StringRef layout, llvm::StringRef file = "main.cpp") {
        return path::join(layouts, layout, file);
    };

    {
        FileTable files;
        CommandTable commands;
        CompilationDatabase database{files, commands};
        ZASSERT(load_layout(database, "single_root") == 1U);
        ZEXPECT(llvm::StringRef(print_argv(render_entry(database, source("single_root"))))
                    .contains("SINGLE"));
    }

    {
        /// A database inside a build directory: `directory` anchors to that
        /// directory and the entry's relative file climbs out of it.
        FileTable files;
        CommandTable commands;
        CompilationDatabase database{files, commands};
        ZASSERT(load_layout(database, "subdir_cdb", "cmake/compile_commands.json") == 1U);
        auto candidates = database.candidate_entries(source("subdir_cdb"));
        ZASSERT(candidates.size() == 1U);
        ZEXPECT(llvm::StringRef(commands.get(candidates.front().command).directory) ==
                Spelling::absolute(path::join(layouts, "subdir_cdb", "cmake")).str());
        ZEXPECT(llvm::StringRef(print_argv(render_entry(database, source("subdir_cdb"))))
                    .contains("OUT"));
    }

    {
        /// One file, two configurations, the Ninja Multi-Config shape.
        FileTable files;
        CommandTable commands;
        CompilationDatabase database{files, commands};
        ZASSERT(load_layout(database, "multi_entry") == 2U);
        auto candidates = database.candidate_entries(source("multi_entry"));
        ZASSERT(candidates.size() == 2U);
        std::string joined;
        for(auto& entry: candidates) {
            joined += print_argv(commands.render_full(entry.command));
            joined += ' ';
        }
        ZEXPECT(llvm::StringRef(joined).contains("CONFIG_A"));
        ZEXPECT(llvm::StringRef(joined).contains("CONFIG_B"));
    }

    {
        FileTable files;
        CommandTable commands;
        CompilationDatabase database{files, commands};
        ZASSERT(load_layout(database, "command_string") == 1U);
        auto argv = print_argv(render_entry(database, source("command_string")));
        ZEXPECT(llvm::StringRef(argv).contains("CMD"));
        ZEXPECT(!llvm::StringRef(argv).contains(" -c "));
        ZEXPECT(!llvm::StringRef(argv).contains(" -o "));
    }

    {
        FileTable files;
        CommandTable commands;
        CompilationDatabase database{files, commands};
        ZASSERT(load_layout(database, "relative_directory") == 1U);
        auto file = source("relative_directory", "src/main.cpp");
        auto candidates = database.candidate_entries(file);
        ZASSERT(candidates.size() == 1U);
        ZEXPECT(llvm::StringRef(commands.get(candidates.front().command).directory) ==
                Spelling::absolute(path::join(layouts, "relative_directory", "src")).str());
        ZEXPECT(llvm::StringRef(print_argv(render_entry(database, file))).contains("RELATIVE"));
    }

    {
        FileTable files;
        CommandTable commands;
        CompilationDatabase database{files, commands};
        ZASSERT(load_layout(database, "launcher_prefix") == 1U);
        auto candidates = database.candidate_entries(source("launcher_prefix"));
        ZASSERT(candidates.size() == 1U);
        auto argv = render_entry(database, source("launcher_prefix"));
        ZASSERT(!argv.empty());
        ZEXPECT(argv.front() == "clang++"sv);
        ZEXPECT(llvm::StringRef(print_argv(argv)).contains("WRAPPED"));
    }

    {
        FileTable files;
        CommandTable commands;
        CompilationDatabase database{files, commands};
        ZASSERT(load_layout(database, "response_file") == 1U);
        auto argv = print_argv(render_entry(database, source("response_file")));
        ZEXPECT(llvm::StringRef(argv).contains("FROM_RSP"));
        ZEXPECT(!llvm::StringRef(argv).contains("@flags.rsp"));
    }
};

};  // ZEST_SUITE(Database)

/// path_id that `cdb` assigns to a file under the temp root.
Fid id_of(CompilationDatabase& cdb, TempDir& tmp, llvm::StringRef rel) {
    return cdb.files().intern(Spelling::absolute(path::join(tmp.root.str(), rel)));
}

/// Overwrite compile_commands.json under the temp root (without loading it).
void write_json(TempDir& tmp, llvm::ArrayRef<CDBEntry> entries) {
    tmp.touch("compile_commands.json", build_cdb_json(entries));
}

ZEST_SUITE(ReloadDiff) {

ZEST_CASE(AddedEntry) {
    TempDir tmp;
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
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
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
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
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
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
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
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
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
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
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-fomit-frame-pointer", "-g"}}
    });
    cdb.load(cdb_path);

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {"-fno-omit-frame-pointer", "-ffunction-sections"}}
    });
    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));

    ZEXPECT(diff->empty());
};

ZEST_CASE(OptLevelIsSemantic) {
    // Anchors that -O* is semantic (defines __OPTIMIZE__), not codegen-only:
    // changing the optimization level must be reported as a change.
    TempDir tmp;
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
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
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
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
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
    auto cdb_path = tmp.path("compile_commands.json");

    write_json(tmp,
               {
                   {tmp.root.str(), "a.cpp", {}},
                   {tmp.root.str(), "b.cpp", {}}
    });
    auto diff = cdb.reload_and_diff(cdb.add_source(Spelling::absolute(cdb_path)));

    ZASSERT(diff->added.size() == 2U);
    ZEXPECT(llvm::is_contained(diff->added, id_of(cdb, tmp, "a.cpp")));
    ZEXPECT(llvm::is_contained(diff->added, id_of(cdb, tmp, "b.cpp")));
    ZEXPECT(diff->removed.empty());
    ZEXPECT(diff->changed.empty());
};

ZEST_CASE(CorruptKeepsEntries) {
    // A half-written / corrupt CDB must leave the loaded entries intact and
    // signal failure so the caller retries instead of seeing "no change".
    TempDir tmp;
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
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
    ZEXPECT(llvm::StringRef(print_argv(commands.render_full(candidates.front().command)))
                .contains("-std=c++20"));
};

ZEST_CASE(MissingFileFails) {
    // An unreadable file (deleted, or still locked by the generator) is a
    // failure, not an empty database: entries survive and the caller retries.
    TempDir tmp;
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
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
