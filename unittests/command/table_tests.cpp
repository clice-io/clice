module;

#include "modules/prelude.h"

module clice;

import :command.database;
import :command.edit;
import :command.table;
import :support.shell;
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

ZEST_CASE(ClAliasesRender) {
    /// cl spellings unalias to options a cl-mode driver reads as another
    /// (`-Wall` is its /Wall) or not at all: a cl alias spells them in
    /// place, `/clang:` (appended last by the driver) only when none does.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake",
                         "/fake/a.cpp",
                         "cl.exe /W3 -Wno-unused-variable /J /Zp4 /Iinc /FIpre.h /std:c++17 /c "
                         "/fake/a.cpp"sv);

    auto argv = print_argv(render_entry(database, "/fake/a.cpp"));
    ZEXPECT(llvm::StringRef(argv).contains("/W1 -Wno-unused-variable"));
    ZEXPECT(llvm::StringRef(argv).contains("/J"));
    ZEXPECT(llvm::StringRef(argv).contains("/Zp4"));
    ZEXPECT(llvm::StringRef(argv).contains("/FIpre.h"));
    ZEXPECT(llvm::StringRef(argv).contains("-I /fake/inc"));
    ZEXPECT(llvm::StringRef(argv).contains("/std:c++17"));
    ZEXPECT(!llvm::StringRef(argv).contains("/clang:"));

    /// GCC-style, with no cl spelling at all.
    database.add_command("/fake", "/fake/b.cpp", "cl.exe -std=c++20 /c /fake/b.cpp"sv);
    ZEXPECT(llvm::StringRef(print_argv(render_entry(database, "/fake/b.cpp")))
                .contains("/clang:-std=c++20"));
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
    /// An extensionless file must yield a real (non-null) empty kind, not
    /// a null C string.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "noext", "clang++ -std=c++20 noext"sv);

    auto& entry = database.candidate_entries(fake("noext")).front();
    auto kind = commands.input_kind(entry.command, "noext");
    ZASSERT(kind.value != nullptr);
    ZEXPECT(llvm::StringRef(kind.value).empty());
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
    ZEXPECT(llvm::any_of(commands.pch_inputs(tool),
                         [&](const Spelling& input) { return file_table.find(input) == pch; }));
};

ZEST_CASE(CommandHashSurvivesMove) {
    /// Paths under the workspace — the compiler, the directory, path
    /// options — hash by their portable names: a moved checkout keeps its
    /// commands' identity.
    auto hash = [](const TempDir& workspace) {
        FileTable file_table;
        CommandTable commands;
        CompilationDatabase database{file_table, commands};
        commands.set_workspace_root(CanonicalPath(Spelling::absolute(workspace.root)));
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
    CommandRef ref{entry.file,
                   entry.command,
                   commands.input_kind(entry.command, "main.cpp"),
                   CommandSource::CDBExact};
    auto argv = commands.render_driver(ref, database.spelling(ref.file));

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
    CommandRef ref2{entry2.file,
                    entry2.command,
                    commands2.input_kind(entry2.command, "main.cpp"),
                    CommandSource::CDBExact};
    auto argv2 = commands2.render_driver(ref2, database2.spelling(ref2.file));
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
