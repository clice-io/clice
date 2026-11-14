module clice;

import :command.argument;
import :command.database;
import :command.edit;
import :command.table;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {

namespace {

using namespace std::literals;

ZEST_SUITE(CommandTable) {

ZEST_CASE(SelectorPositional) {
    /// -x only governs inputs after it: a trailing selector leaves the
    /// input to its extension, and removing the leading one restores it.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cu", "clang -x c++ a.cu -x c"sv);

    auto base = database.candidate_entries(fake("a.cu")).front().command;
    ZEXPECT(llvm::StringRef(commands.input_kind(base, "a.cu").value) == "c++");

    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Remove, {"-x", "c++"}}
    };
    CommandEdits options{.edits = edits};
    auto applied = apply_edits(commands, file_table, base, options);
    ZEXPECT(llvm::StringRef(commands.input_kind(applied, "a.cu").value) == "cuda");

    /// -x none resets the state; the extension decides again.
    database.add_command("/fake", "b.c", "clang -x c++ -x none b.c"sv);
    auto reset = database.candidate_entries(fake("b.c")).front().command;
    ZEXPECT(llvm::StringRef(commands.input_kind(reset, "b.c").value) == "c");
};

ZEST_CASE(CxxDriverCInputs) {
    /// g++ and clang++ compile C inputs as C++; gcc does not, and an
    /// explicit driver mode overrides the name.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.c", "/usr/bin/x86_64-linux-gnu-g++-13 -c a.c"sv);
    database.add_command("/fake", "b.h", "clang++ -c b.h"sv);
    database.add_command("/fake", "c.c", "gcc -c c.c"sv);
    database.add_command("/fake", "d.c", "clang++ --driver-mode=gcc -c d.c"sv);
    database.add_command("/fake", "e.c", "clang --driver-mode=g++ -c e.c"sv);

    auto kind = [&](llvm::StringRef file) {
        auto command = database.candidate_entries(fake(file)).front().command;
        return llvm::StringRef(commands.input_kind(command, file).value);
    };
    ZEXPECT(kind("a.c") == "c++");
    ZEXPECT(kind("b.h") == "c++-header");
    ZEXPECT(kind("c.c") == "c");
    ZEXPECT(kind("d.c") == "c");
    ZEXPECT(kind("e.c") == "c++");
};

ZEST_CASE(IdentityHashes) {
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cpp", "clang++ -std=c++20 a.cpp"sv);
    database.add_command("/fake", "b.cpp", "clang++ -std=c++20 b.cpp"sv);
    database.add_command("/fake", "c.cpp", "clang++ -std=c++17 c.cpp"sv);
    database.add_command("/fake", "x1.c", "clang -x c++ x1.c"sv);
    database.add_command("/fake", "x2.c", "clang -x c x2.c"sv);
    database.add_command("/fake", "g.cpp", "clang++ -std=c++20 -g a.cpp"sv);

    auto hash_of = [&](llvm::StringRef file) {
        return commands.command_hash(database.candidate_entries(fake(file)).front().command);
    };

    /// Same command, different file: one command, one identity.
    ZEXPECT(hash_of("a.cpp") == hash_of("b.cpp"));
    /// A semantic flag difference changes the identity.
    ZEXPECT(hash_of("a.cpp") != hash_of("c.cpp"));
    /// A selector difference changes the identity (selectors live in args).
    ZEXPECT(hash_of("x1.c") != hash_of("x2.c"));
    /// Codegen-only flags never enter the identity.
    ZEXPECT(hash_of("a.cpp") == hash_of("g.cpp"));
    /// Driver-ignored flags never enter the identity.
    database.add_command("/fake", "r.cpp", "clang++ -std=c++20 -frandom-seed=r.o r.cpp"sv);
    ZEXPECT(hash_of("a.cpp") == hash_of("r.cpp"));

    /// The input slot's position is part of the identity.
    database.add_command("/fake", "p1.cpp", "clang++ p1.cpp -Wall"sv);
    database.add_command("/fake", "p2.cpp", "clang++ -Wall p2.cpp"sv);
    ZEXPECT(hash_of("p1.cpp") != hash_of("p2.cpp"));

    ZEXPECT(commands.command_hash_hex(database.candidate_entries(fake("a.cpp")).front().command)
                .size() == 16U);
};

ZEST_CASE(InputKindNoExtension) {
    /// A file with no extension is a header only an include reads: it
    /// compiles as a header of the driver's language, never as linker input.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cpp", "clang++ -std=c++20 a.cpp"sv);
    database.add_command("/fake", "b.c", "clang b.c"sv);
    auto cxx = database.candidate_entries(fake("a.cpp")).front().command;
    auto c = database.candidate_entries(fake("b.c")).front().command;
    ZEXPECT(llvm::StringRef(commands.input_kind(cxx, "/usr/include/c++/vector").value) ==
            "c++-header");
    ZEXPECT(llvm::StringRef(commands.input_kind(c, "/fake/config").value) == "c-header");
};

ZEST_CASE(WorkingDirectoryPCHOutput) {
    /// The precompiled header a command writes is spelled where the
    /// compile runs, as the one a command reads is, and the one GCC finds
    /// beside a forced header.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command(
        "/project",
        "/project/pch.cxx",
        "clang++ -working-directory build -x c++-header -o app.pch /project/pch.cxx"sv);
    database.add_command("/project",
                         "/project/main.cpp",
                         "clang++ -Xclang -include-pch -Xclang build/app.pch main.cpp"sv);
    auto main = database.candidate_entries("/project/main.cpp").front().command;
    auto inputs = commands.pch_inputs(main);
    ZASSERT(inputs.size() == 1U);
    auto pch = file_table.find(inputs.front());
    ZASSERT(pch);
    ZEXPECT(database.pch_writers(*pch).size() == 1U);

    database.add_command("/project",
                         "/project/tool.cpp",
                         "clang++ -working-directory build -include app /project/tool.cpp"sv);
    auto tool = database.candidate_entries("/project/tool.cpp").front().command;
    ZEXPECT(ranges::any_of(commands.pch_inputs(tool),
                           [&](const Spelling& input) { return file_table.find(input) == pch; }));
};

ZEST_CASE(ClPCHOptions) {
    /// cl's `/Yc` writes the PCH `/Fp` names and `/Yu` reads it, as CMake
    /// writes them for MSVC; without `/Fp`, the through header's name with
    /// `.pch` for its extension.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/project",
                         "/project/cmake_pch.cxx",
                         R"(cl /TP /Yc/project/cmake_pch.hxx /FpCMakeFiles/app.pch )"
                         R"(/FI/project/cmake_pch.hxx /project/cmake_pch.cxx)"sv);
    database.add_command("/project",
                         "/project/main.cpp",
                         R"(cl /Yu/project/cmake_pch.hxx /FpCMakeFiles/app.pch )"
                         R"(/FI/project/cmake_pch.hxx main.cpp)"sv);
    database.add_command("/project", "/project/stdafx.cpp", "cl /Ycstdafx.h stdafx.cpp"sv);
    database.add_command("/project", "/project/off.cpp", "cl /Ycstdafx.h /Y- off.cpp"sv);
    auto command = [&](llvm::StringRef file) {
        return database.candidate_entries(file).front().command;
    };
    auto pch = commands.pch_output(command("/project/cmake_pch.cxx"));
    ZASSERT(pch);
    ZEXPECT(CanonicalPath(*pch) == CanonicalPath(Spelling::absolute("/project/CMakeFiles/app.pch")));
    auto read = file_table.find(*pch);
    ZASSERT(read);
    ZEXPECT(database.pch_writers(*read).size() == 1U);
    ZEXPECT(ranges::any_of(commands.pch_inputs(command("/project/main.cpp")),
                           [&](const Spelling& input) { return file_table.find(input) == read; }));

    auto stdafx = commands.pch_output(command("/project/stdafx.cpp"));
    ZASSERT(stdafx);
    ZEXPECT(CanonicalPath(*stdafx) == CanonicalPath(Spelling::absolute("/project/stdafx.pch")));
    ZEXPECT(!commands.pch_output(command("/project/off.cpp")).has_value());
};

ZEST_CASE(CommandHashSurvivesMove) {
    /// Paths under the workspace — the compiler, the directory, path
    /// options — hash by their portable names: a moved checkout keeps its
    /// commands' identity.
    auto hash = [](const TempDir& workspace) {
        FileTable file_table;
        CommandTable commands;
        CompilationDatabase database{file_table, commands};
        commands.set_project_root(CanonicalPath(Spelling::absolute(workspace.root)));
        auto driver = workspace.path("tools/clang++");
        auto include = "-I" + workspace.path("inc");
        auto entry = database.add_command(workspace.root,
                                          workspace.path("main.cpp"),
                                          {driver.c_str(), include.c_str(), "main.cpp"});
        return commands.command_hash(entry->command);
    };
    TempDir before;
    TempDir after;
    ZEXPECT(hash(before) == hash(after));
};

ZEST_CASE(ModuleOutputsShare) {
    /// Where a module unit writes its BMI leaves the command, whether it
    /// writes one stays: that decides the BMI a precompile writes.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "m1.cppm", "clang++ -std=c++20 -fmodule-output=m1.pcm m1.cppm"sv);
    database.add_command("/fake", "m2.cppm", "clang++ -std=c++20 -fmodule-output=m2.pcm m2.cppm"sv);
    database.add_command("/fake", "m3.cppm", "clang++ -std=c++20 -fmodule-output m3.cppm"sv);
    database.add_command("/fake", "m4.cppm", "clang++ -std=c++20 m4.cppm"sv);
    auto command = [&](llvm::StringRef file) {
        return database.candidate_entries(fake(file)).front().command;
    };
    ZEXPECT(command("m1.cppm") == command("m2.cppm"));
    ZEXPECT(command("m1.cppm") == command("m3.cppm"));
    ZEXPECT(command("m1.cppm") != command("m4.cppm"));
    ZEXPECT(commands.command_hash(command("m1.cppm")) != commands.command_hash(command("m4.cppm")));
};

ZEST_CASE(PCHWriterKeepsOutput) {
    /// An output stays only where it names the precompiled header a command
    /// writes.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cxx", "clang++ -x c++-header -o a.pch -c a.cxx"sv);
    database.add_command("/fake",
                         "b.cpp",
                         "clang++ -Xclang -emit-pch -o first.pch -o b.pch -c b.cpp"sv);
    database.add_command("/fake", "c.cpp", "clang++ -o c.o -c c.cpp"sv);
    database.add_command("/fake", "d.cpp", "clang++ -o d.o -c d.cpp"sv);
    database.add_command("/fake",
                         "e.h",
                         "clang++ -x c++-header -fsyntax-only -o e.h.processed e.h"sv);
    auto command = [&](llvm::StringRef file) {
        return database.candidate_entries(fake(file)).front().command;
    };
    auto writes = [&](llvm::StringRef file, llvm::StringRef pch) {
        auto output = commands.pch_output(command(file));
        return output && CanonicalPath(*output) == CanonicalPath(Spelling::absolute(fake(pch)));
    };
    ZEXPECT(writes("a.cxx", "a.pch"));
    ZEXPECT(writes("b.cpp", "b.pch"));
    ZEXPECT(!commands.pch_output(command("c.cpp")).has_value());
    ZEXPECT(!commands.pch_output(command("e.h")).has_value());
    ZEXPECT(command("c.cpp") == command("d.cpp"));
};

ZEST_CASE(OrdinaryHashStable) {
    /// Every index records its unit's command hash: a change to the bytes
    /// of an ordinary command's identity reindexes every project.
    TempDir workspace;
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    commands.set_project_root(CanonicalPath(Spelling::absolute(workspace.root)));
    auto include = "-I" + workspace.path("inc");
    auto entry = database.add_command(workspace.root,
                                      workspace.path("main.cpp"),
                                      {"clang++",
                                       "-std=c++20",
                                       "-O2",
                                       "-g",
                                       "-Wall",
                                       "-DNDEBUG",
                                       include.c_str(),
                                       "-c",
                                       "-o",
                                       "main.o",
                                       "main.cpp"});
    ZEXPECT(commands.command_hash(entry->command) == 0x49934bc992945eb8ULL);
};

ZEST_CASE(ForcedLanguage) {
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.h", "clang++ -x c++ a.h"sv);
    database.add_command("/fake", "b.cpp", "clang++ b.cpp"sv);

    ZEXPECT(commands.forced_language(database.candidate_entries(fake("a.h")).front().command) ==
            "c++");
    ZEXPECT(commands.forced_language(database.candidate_entries(fake("b.cpp")).front().command)
                .empty());
};

ZEST_CASE(ResourceDir) {
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "main.cpp", "clang++ -std=c++23 test.cpp"sv);

    auto& entry = database.candidate_entries(fake("main.cpp")).front();
    auto ref = database_command(database, entry);
    auto argv = commands.render_driver(ref, database.files().spelling(ref.file));

    bool has_resource_dir = false;
    for(std::size_t i = 0; i + 1 < argv.size(); i += 1) {
        if(argv[i] == "-resource-dir"sv) {
            ZEXPECT(llvm::StringRef(argv[i + 1]) == resource_dir());
            has_resource_dir = true;
            break;
        }
    }
    ZEXPECT(has_resource_dir == !resource_dir().empty());

    /// A command carrying its own resource dir is not double-injected.
    FileTable file_table2;
    CommandTable commands2;
    CompilationDatabase database2{file_table2, commands2};
    database2.add_command("/fake", "main.cpp", "clang++ -resource-dir /custom main.cpp"sv);
    auto& entry2 = database2.candidate_entries(fake("main.cpp")).front();
    auto ref2 = database_command(database2, entry2);
    auto argv2 = commands2.render_driver(ref2, database2.files().spelling(ref2.file));
    int count = 0;
    for(auto* arg: argv2) {
        if(arg == "-resource-dir"sv) {
            count += 1;
        }
    }
    ZEXPECT(count == 1);
};

};  // ZEST_SUITE(CommandTable)

}  // namespace

}  // namespace clice::testing
