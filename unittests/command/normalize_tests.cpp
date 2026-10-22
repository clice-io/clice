module;

#include "modules/prelude.h"

module clice;

import :command.argument;
import :command.database;
import :command.normalize;
import :command.table;
import :support.shell;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {

namespace {

using namespace std::literals;

#define EXPECT_CONTAINS(haystack, needle) ZEXPECT(llvm::StringRef(haystack).contains(needle))
#define EXPECT_NOT_CONTAINS(haystack, needle) ZEXPECT(!llvm::StringRef(haystack).contains(needle))

ZEST_SUITE(Normalize) {

/// `result` spells the rendered input `{}`.
void EXPECT_STRIP(llvm::StringRef argv, std::string_view result) {
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    auto file = fake("main.cpp");
    database.add_command("/fake", "main.cpp", argv);
    ZASSERT(std::vformat(result, std::make_format_args(file)) ==
            print_argv(render_entry(database, file)));
};

ZEST_CASE(DefaultFilters) {
    /// Filter -c, -o and keep the input in place.
    EXPECT_STRIP("g++ main.cpp", "g++ {}");
    EXPECT_STRIP("clang++ -c main.cpp", "clang++ {}");
    EXPECT_STRIP("clang++ -o main.o main.cpp", "clang++ {}");
    EXPECT_STRIP("clang++ -c -o main.o main.cpp", "clang++ {}");
    EXPECT_STRIP("cl.exe /c /Fomain.cpp.o main.cpp", "cl.exe {}");
    /// CL options stay visible under Windows's free-form driver casing.
    EXPECT_STRIP("CL.exe /FIfoo.h /c main.cpp", "CL.exe /FIfoo.h {}");

    /// Filter PCH related.

    /// CMake
    EXPECT_STRIP("g++ -std=gnu++20 -Winvalid-pch -include cmake_pch.hxx -o main.cpp.o -c main.cpp",
                 "g++ -std=gnu++20 -Winvalid-pch -include cmake_pch.hxx {}");
    EXPECT_STRIP(
        "clang++ -Winvalid-pch -Xclang -include-pch -Xclang cmake_pch.hxx.pch -Xclang -include -Xclang cmake_pch.hxx -o main.cpp.o -c main.cpp",
        "clang++ -Winvalid-pch -Xclang -include -Xclang cmake_pch.hxx {}");
    EXPECT_STRIP("cl.exe /Yufoo.h /FIfoo.h /Fpfoo.h_v143.pch /c /Fomain.cpp.o main.cpp",
                 "cl.exe /FIfoo.h {}");
    EXPECT_STRIP("cl.exe /Ycfoo.h /FIfoo.h /Fpfoo.h_v143.pch /c /Fomain.cpp.o main.cpp",
                 "cl.exe /FIfoo.h {}");
};

ZEST_CASE(CommandDedup) {
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "test.cpp", "clang++ -std=c++23 test.cpp"sv);
    database.add_command("/fake", "test2.cpp", "clang++ -std=c++23 test2.cpp"sv);
    database.add_command("/fake", "test3.cpp", "clang++ -std=c++23 -DA test3.cpp"sv);

    /// Same flags dedupe to one config; a user-content difference splits.
    auto config1 = database.candidate_entries(fake("test.cpp")).front().command;
    auto config2 = database.candidate_entries(fake("test2.cpp")).front().command;
    auto config3 = database.candidate_entries(fake("test3.cpp")).front().command;
    ZEXPECT(config1 == config2);
    ZEXPECT(config1 != config3);

    auto argv1 = render_entry(database, fake("test.cpp"));
    ZASSERT(argv1.size() == 3U);
    ZEXPECT(argv1[0] == "clang++"sv);
    ZEXPECT(argv1[1] == "-std=c++23"sv);
    ZEXPECT(argv1[2] == fake("test.cpp"));
    ZEXPECT(render_entry(database, fake("test2.cpp")).back() == fake("test2.cpp"));
};

ZEST_CASE(PerFileClSelectors) {
    /// /Tc<file> and /Tp<file> pair a selector with one input: the entry's
    /// own selector rewrites to the equivalent global form, the other
    /// input vanishes with its selector.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "/fake/alpha.c", "cl /Tc/fake/alpha.c /Tp/fake/beta.c /c"sv);
    database.add_command("/fake", "/fake/beta.c", "cl /Tc/fake/alpha.c /Tp/fake/beta.c /c"sv);

    auto alpha = database.candidate_entries("/fake/alpha.c").front().command;
    auto beta = database.candidate_entries("/fake/beta.c").front().command;
    ZEXPECT(alpha != beta);

    ZEXPECT(llvm::StringRef(commands.input_kind(alpha, "/fake/alpha.c").value) == "c");
    ZEXPECT(llvm::StringRef(commands.input_kind(beta, "/fake/beta.c").value) == "c++");

    auto beta_argv = print_argv(render_entry(database, "/fake/beta.c"));
    EXPECT_CONTAINS(beta_argv, "/TP");
    EXPECT_NOT_CONTAINS(beta_argv, "alpha.c");

    auto alpha_argv = print_argv(render_entry(database, "/fake/alpha.c"));
    EXPECT_CONTAINS(alpha_argv, "/TC");
    EXPECT_NOT_CONTAINS(alpha_argv, "beta.c");
};

ZEST_CASE(WrapperStripped) {
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cpp", "ccache clang++ -std=c++20 a.cpp"sv);
    database.add_command("/fake", "b.cpp", "clang++ -std=c++20 b.cpp"sv);

    /// The wrapper is stripped and takes no part in the config identity.
    auto& a = database.candidate_entries(fake("a.cpp")).front();
    auto& b = database.candidate_entries(fake("b.cpp")).front();
    ZEXPECT(a.command == b.command);

    ZEXPECT(llvm::StringRef(commands.get(a.command).driver) == "clang++");
    EXPECT_NOT_CONTAINS(print_argv(render_entry(database, fake("a.cpp"))), "ccache");
};

ZEST_CASE(WrapperValueOptions) {
    /// A wrapper option's separate KEY=VAL value must not be mistaken for
    /// the compiler.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake",
                         "a.cpp",
                         "ccache --set-config cache_dir=/tmp/cc clang++ -std=c++20 a.cpp"sv);
    database.add_command("/fake", "b.cpp", "clang++ -std=c++20 b.cpp"sv);

    auto& a = database.candidate_entries(fake("a.cpp")).front();
    auto& b = database.candidate_entries(fake("b.cpp")).front();
    ZEXPECT(a.command == b.command);
    ZEXPECT(llvm::StringRef(commands.get(a.command).driver) == "clang++");
};

ZEST_CASE(WrapperCaseInsensitive) {
    /// Windows tools emit launcher spellings like CCACHE.EXE.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cpp", "CCACHE.EXE clang++ -std=c++20 a.cpp"sv);

    auto& a = database.candidate_entries(fake("a.cpp")).front();
    ZEXPECT(llvm::StringRef(commands.get(a.command).driver) == "clang++");
};

ZEST_CASE(ResponseFileExpansion) {
    TempDir tmp;
    tmp.touch("flags.rsp", "-std=c++23 -DFROM_RSP=1\n");
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command(tmp.root.str(), "main.cpp", "clang++ @flags.rsp main.cpp"sv);

    auto argv = print_argv(render_entry(database, tmp.path("main.cpp")));
    EXPECT_CONTAINS(argv, "-std=c++23");
    EXPECT_CONTAINS(argv, "FROM_RSP=1");
    EXPECT_NOT_CONTAINS(argv, "@");
};

ZEST_CASE(DriverModeFromRsp) {
    /// --driver-mode=cl inside a response file still switches on CL option
    /// visibility (clang interprets the mode after expansion).
    TempDir tmp;
    tmp.touch("flags.rsp", "--driver-mode=cl /TP\n");
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command(tmp.root.str(), "main.cpp", "clang++ @flags.rsp main.cpp"sv);

    EXPECT_CONTAINS(print_argv(render_entry(database, tmp.path("main.cpp"))), "/TP");
};

ZEST_CASE(InternedCommand) {
    /// A hand-written command normalizes like an entry: one CommandID per
    /// (directory, spelling), the string and argv forms meeting on it, the
    /// input slot synthesized at the end.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    Normalizer normalizer{.commands = commands, .files = file_table};
    auto intern = [&](llvm::StringRef directory, llvm::ArrayRef<const char*> arguments) {
        return normalizer.normalize(Spelling::absolute(directory), Fid{}, arguments);
    };
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver saver(allocator);
    auto spelled = *intern("/ws", tokenize_command("clang++ -std=c++20 -Iinclude", saver));
    llvm::SmallVector<const char*> argv = {"clang++", "-std=c++20", "-Iinclude"};
    ZEXPECT(spelled == *intern("/ws", argv));
    ZEXPECT(spelled != *intern("/other", argv));

    /// Spellings that name no compiler are rejected, not asserted on.
    ZEXPECT(!intern("/ws", {}).has_value());
    ZEXPECT(!intern("/ws", {"ccache"}).has_value());

    CommandRef ref{file_table.intern(Spelling::absolute("/ws/src/a.cpp")),
                   spelled,
                   commands.input_kind(spelled, "/ws/src/a.cpp"),
                   CommandSource::Default};
    auto rendered = commands.render_driver(ref, database.spelling(ref.file));
    ZEXPECT(llvm::is_contained(rendered, "-std=c++20"sv));
    ZEXPECT(has_arg(rendered, "/ws/include"));
    ZEXPECT(std::string_view(rendered.back()) == file_table.resolve(ref.file).str());
};

ZEST_CASE(CodegenFilter) {
    /// Codegen-only options never reach the compile render.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command(
        "/fake",
        "main.cpp",
        "clang++ -std=c++20 -fPIC -fno-omit-frame-pointer -fstack-protector-strong "
        "-fdata-sections -ffunction-sections -fcolor-diagnostics -g main.cpp"sv);

    auto argv = print_argv(render_entry(database, fake("main.cpp")));

    EXPECT_CONTAINS(argv, "-std=c++20");
    /// They define __PIC__ and __SSP_STRONG__.
    EXPECT_CONTAINS(argv, "-fPIC");
    EXPECT_CONTAINS(argv, "-fstack-protector-strong");

    EXPECT_NOT_CONTAINS(argv, "-fno-omit-frame-pointer");
    EXPECT_NOT_CONTAINS(argv, "-fdata-sections");
    EXPECT_NOT_CONTAINS(argv, "-ffunction-sections");
    EXPECT_NOT_CONTAINS(argv, "-fcolor-diagnostics");
    EXPECT_NOT_CONTAINS(argv, "-g");

    /// They survive in the full build.
    auto full = print_argv(
        commands.render_full(database.candidate_entries(fake("main.cpp")).front().command));
    EXPECT_CONTAINS(full, "-fno-omit-frame-pointer");
    EXPECT_CONTAINS(full, "-fdata-sections");
};

ZEST_CASE(FlangOnlyWarnings) {
    /// Spellings flang ignores are clang's warning options.
    EXPECT_STRIP("clang++ -Wall -Wextra -Wconversion main.cpp",
                 "clang++ -Wall -Wextra -Wconversion {}");
    EXPECT_STRIP("clang-cl /W4 -Wextra /c main.cpp", "clang-cl /W4 -Wextra {}");
};

ZEST_CASE(GCCOnlyWarnings) {
    /// A GCC command's warnings clang does not know drop; those it knows
    /// stay, in every form clang reads.
    EXPECT_STRIP(
        "g++ -Wall -Wlogical-op -Wno-useless-cast -Werror=duplicated-cond "
        "-Wformat=2 -Wno-error=unused-variable -Werror -Wfatal-errors main.cpp",
        "g++ -Wall -Wformat=2 -Wno-error=unused-variable -Werror -Wfatal-errors {}");
    /// Clang's own commands keep them: clang itself reports them.
    EXPECT_STRIP("clang++ -Wlogical-op main.cpp", "clang++ -Wlogical-op {}");
};

ZEST_CASE(LTOModes) {
    /// The LTO modes clang takes stay; g++'s job count goes.
    EXPECT_STRIP("clang++ -flto main.cpp", "clang++ -flto=full {}");
    EXPECT_STRIP("clang++ -flto=thin main.cpp", "clang++ -flto=thin {}");
    EXPECT_STRIP("clang++ -flto=auto main.cpp", "clang++ -flto=full {}");
    EXPECT_STRIP("g++ -flto=jobserver main.cpp", "g++ -flto=full {}");
    EXPECT_STRIP("g++ -flto=8 main.cpp", "g++ {}");
};

ZEST_CASE(DependencyScanFilter) {
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake",
                         "main.cpp",
                         "clang++ -std=c++20 -MD -MF main.d -MT main.o main.cpp"sv);

    auto argv = print_argv(render_entry(database, fake("main.cpp")));

    EXPECT_CONTAINS(argv, "-std=c++20");
    EXPECT_NOT_CONTAINS(argv, "-MD");
    EXPECT_NOT_CONTAINS(argv, "-MF");
    EXPECT_NOT_CONTAINS(argv, "-MT");
    EXPECT_NOT_CONTAINS(argv, "main.d");
};

ZEST_CASE(PreprocessorDependencyFilter) {
    /// Kbuild hands the preprocessor its dependency file through -Wp.
    EXPECT_STRIP("gcc -Wp,-MMD,arch/x86/.main.o.d -std=gnu11 main.cpp", "gcc -std=gnu11 {}");
    EXPECT_STRIP("clang++ -Wp,-MD,main.d -Wp,-MT,main.o,-MP -Wp,-MQmain.o main.cpp", "clang++ {}");
    /// What else a -Wp hands over keeps it.
    EXPECT_STRIP("g++ -Wp,-D_FORTIFY_SOURCE=2 main.cpp", "g++ -Wp,-D_FORTIFY_SOURCE=2 {}");
    EXPECT_STRIP("g++ -Wp,-MMD,main.d,-DNDEBUG main.cpp", "g++ -Wp,-MMD,main.d,-DNDEBUG {}");
    EXPECT_STRIP("g++ -Wp,-fclice-unknown main.cpp", "g++ -Wp,-fclice-unknown {}");
    EXPECT_STRIP("g++ -Wp,-include -Wp,config.h main.cpp", "g++ -Wp,-include -Wp,config.h {}");
};

ZEST_CASE(ModuleFilter) {
    /// A named module mapping is discarded (clice builds its own PCMs);
    /// the bare header-unit form stays part of the frontend semantics.
    EXPECT_STRIP("clang++ -std=c++20 -fmodule-file=m=mod.pcm main.cpp", "clang++ -std=c++20 {}");
    EXPECT_STRIP("clang++ -std=c++20 -fmodule-file=mod.pcm main.cpp",
                 "clang++ -std=c++20 -fmodule-file=mod.pcm {}");
    EXPECT_STRIP("clang++ -std=c++20 -fprebuilt-module-path=/tmp main.cpp",
                 "clang++ -std=c++20 {}");
};

ZEST_CASE(UserContentClassification) {
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cpp", "clang++ -std=c++20 -Wall -DA=1 -DFOO a.cpp"sv);
    database.add_command("/fake", "b.cpp", "clang++ -std=c++20 -Wall -DB=2 b.cpp"sv);

    auto a_argv = print_argv(render_entry(database, fake("a.cpp")));
    auto b_argv = print_argv(render_entry(database, fake("b.cpp")));

    EXPECT_CONTAINS(a_argv, "-std=c++20");
    EXPECT_CONTAINS(a_argv, "-Wall");
    EXPECT_CONTAINS(b_argv, "-std=c++20");
    EXPECT_CONTAINS(b_argv, "-Wall");

    EXPECT_CONTAINS(a_argv, "A=1");
    EXPECT_CONTAINS(a_argv, "FOO");
    EXPECT_CONTAINS(b_argv, "B=2");

    EXPECT_NOT_CONTAINS(a_argv, "B=2");
    EXPECT_NOT_CONTAINS(b_argv, "A=1");
};

ZEST_CASE(IncludePathAbsolutize) {
    /// Relative include paths absolutize against the entry directory.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/project/build",
                         "main.cpp",
                         "clang++ -Iinclude -isystem sys/inc -iquote ../src main.cpp"sv);

    auto result = render_entry(database, "/project/build/main.cpp");
    ZEXPECT(has_arg(result, "/project/build/include"));
    ZEXPECT(has_arg(result, "/project/build/sys/inc"));
    ZEXPECT(has_arg(result, "/project/"));

    /// Absolute paths are kept as-is.
    FileTable file_table2;
    CommandTable commands2;
    CompilationDatabase database2{file_table2, commands2};
    database2.add_command("/project/build", "main.cpp", "clang++ -I/usr/include main.cpp"sv);
    ZEXPECT(has_arg(render_entry(database2, "/project/build/main.cpp"), "/usr/include"));
};

ZEST_CASE(WorkingDirectoryAnchorsIncludes) {
    /// Relative include paths resolve where the compile runs: the entry
    /// directory, moved by -working-directory.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/project",
                         "main.cpp",
                         "clang++ -working-directory build -Iinclude main.cpp"sv);
    ZEXPECT(has_arg(render_entry(database, "/project/main.cpp"), "/project/build/include"));
    /// The directory itself is anchored too, so no reader depends on
    /// where it runs.
    auto config = database.candidate_entries("/project/main.cpp").front().command;
    auto working = llvm::find_if(commands.get(config).args, [](const Arg& arg) {
        return arg.opt_id == option::OPT_working_directory;
    });
    ZASSERT(working != commands.get(config).args.end());
    auto value = path::convert_to_slash(working->values[0]);
    ZEXPECT((path::is_rooted(value) && llvm::StringRef(value).ends_with("/project/build")));
};

ZEST_CASE(BareFileValueAnchored) {
    /// A file an option reads is relative to the entry directory with or
    /// without a separator; the toolchain probe runs elsewhere. A bare
    /// `--config` name is clang's to search in its configuration
    /// directories.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/project",
                         "main.cpp",
                         "clang++ -fsanitize-ignorelist=ignore.txt --config=x.cfg --sysroot sdk "
                         "main.cpp"sv);
    auto config = database.candidate_entries("/project/main.cpp").front().command;
    llvm::SmallVector<std::string> values;
    for(auto& arg: commands.get(config).args) {
        for(llvm::StringRef value: arg.values) {
            values.push_back(path::convert_to_slash(value));
        }
    }
    ZEXPECT(llvm::is_contained(values, "/project/ignore.txt"));
    ZEXPECT(llvm::is_contained(values, "/project/sdk"));
    ZEXPECT(llvm::is_contained(values, "x.cfg"));
};

ZEST_CASE(SysrootIncludeKept) {
    /// A leading `=` names the sysroot, which clang substitutes.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/project",
                         "main.cpp",
                         "clang++ --sysroot=/sdk -I=/usr/include/foo main.cpp"sv);
    auto argv = render_entry(database, "/project/main.cpp");
    ZEXPECT(llvm::any_of(argv, [](llvm::StringRef arg) {
        return arg == "=/usr/include/foo" || arg == "-I=/usr/include/foo";
    }));
};

ZEST_CASE(SemanticOptionsPreserved) {
    EXPECT_STRIP("clang++ -std=c++20 -fno-exceptions -fno-rtti -pedantic main.cpp",
                 "clang++ -std=c++20 -fno-exceptions -fno-rtti -pedantic {}");
    EXPECT_STRIP("clang++ -std=c++20 -Wall -Werror main.cpp",
                 "clang++ -std=c++20 -Wall -Werror {}");
};

};  // ZEST_SUITE(Normalize)

}  // namespace

}  // namespace clice::testing
