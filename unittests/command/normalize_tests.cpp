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

ZEST_SUITE(Normalize) {

ZEST_CASE(CommandDedup) {
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "test.cpp", "clang++ -std=c++23 test.cpp"sv);
    database.add_command("/fake", "test2.cpp", "clang++ -std=c++23 test2.cpp"sv);
    database.add_command("/fake", "test3.cpp", "clang++ -std=c++23 -DA test3.cpp"sv);

    /// Same flags dedupe to one command; a user-content difference splits.
    auto command1 = database.candidate_entries(fake("test.cpp")).front().command;
    auto command2 = database.candidate_entries(fake("test2.cpp")).front().command;
    auto command3 = database.candidate_entries(fake("test3.cpp")).front().command;
    ZEXPECT(command1 == command2);
    ZEXPECT(command1 != command3);

    auto argv1 = render_entry(database, fake("test.cpp"));
    ZASSERT(argv1.size() == 3U);
    ZEXPECT(argv1[0] == "clang++"sv);
    ZEXPECT(argv1[1] == "-std=c++23"sv);
    ZEXPECT(argv1[2] == fake("test.cpp"));
    ZEXPECT(render_entry(database, fake("test2.cpp")).back() == fake("test2.cpp"));
};

ZEST_CASE(PerFileClSelectors) {
    /// /Tc<file> and /Tp<file> pair a selector with one input: each entry
    /// is a command of its own, compiling its input in that input's
    /// language.
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
};

ZEST_CASE(WrapperStripped) {
    /// A compiler launcher takes no part in the command identity, nor do
    /// its own options, chained launchers, or ccache's guard of the token
    /// after it.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cpp", "ccache clang++ -std=c++20 a.cpp"sv);
    database.add_command("/fake", "b.cpp", "clang++ -std=c++20 b.cpp"sv);
    database.add_command("/fake",
                         "c.cpp",
                         "ccache --set-config cache_dir=/tmp/cc clang++ -std=c++20 c.cpp"sv);
    database.add_command("/fake", "d.cpp", "sccache clang++ -std=c++20 d.cpp"sv);
    database.add_command("/fake", "e.cpp", "distcc icecc clang++ -std=c++20 e.cpp"sv);
    database.add_command("/fake", "f.cpp", "ccache clang++ --ccache-skip -std=c++20 f.cpp"sv);

    auto b = database.candidate_entries(fake("b.cpp")).front().command;
    for(auto file: {"a.cpp", "c.cpp", "d.cpp", "e.cpp", "f.cpp"}) {
        ZEXPECT(database.candidate_entries(fake(file)).front().command == b);
    }
};

ZEST_CASE(SharedKeys) {
    /// Entries a build compiles alike share a key, as their commands are
    /// one; commands that differ, even where a spelling looks alike, do not.
    FileTable file_table;
    CommandTable commands;
    Normalizer normalizer{.commands = commands, .files = file_table};
    auto directory = Spelling::absolute("/ws");
    auto key = [&](llvm::StringRef file, llvm::StringRef line) {
        return normalizer.shared_key(directory, file, split_command(line));
    };
    auto command = [&](llvm::StringRef file, llvm::StringRef line) {
        return normalizer.normalize(directory, file_table.intern(Spelling(file, directory)), line);
    };
    auto same = [&](std::string_view line) {
        std::string a_name = "a";
        std::string b_name = "b";
        auto a = std::vformat(line, std::make_format_args(a_name));
        auto b = std::vformat(line, std::make_format_args(b_name));
        auto a_key = key("a.cpp", a);
        return a_key && a_key == key("b.cpp", b) && command("a.cpp", a) == command("b.cpp", b);
    };
    ZEXPECT(same("clang++ -std=c++20 -o {0}.o -c {0}.cpp"));
    ZEXPECT(same("g++ -MD -MF {0}.d -frandom-seed={0}.o -c {0}.cpp -o {0}.o"));
    ZEXPECT(same("gcc -Wp,-MMD,.{0}.o.d -c -o {0}.o {0}.cpp"));
    ZEXPECT(same("cl /std:c++20 /Fo{0}.obj /c {0}.cpp"));

    ZEXPECT((key("a.cpp", "cl -MT /c a.cpp") != key("b.cpp", "cl -MTd /c b.cpp")));
    ZEXPECT((key("a.cpp", "gcc -Wp,-MMD,a.d,-DA -c a.cpp") !=
             key("b.cpp", "gcc -Wp,-MMD,b.d,-DA -c b.cpp")));
    ZEXPECT((key("a.cpp", "clang++ -std=c++17 a.cpp") != key("b.cpp", "clang++ -std=c++20 b.cpp")));

    /// Commands reading files of their own, or naming theirs twice, have none.
    ZEXPECT(!key("a.cpp", "clang++ @a.rsp a.cpp").has_value());
    ZEXPECT(!key("a.cu", "nvcc -c a.cu").has_value());
    ZEXPECT(!key("a.cpp", "clang++ -include a.cpp a.cpp").has_value());
};

ZEST_CASE(InternedCommand) {
    /// A hand-written command normalizes like an entry: one CommandID per
    /// (directory, spelling), the string and argv forms meeting on it, the
    /// input slot synthesized at the end.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    Normalizer normalizer{.commands = commands, .files = file_table};
    auto intern = [&](llvm::StringRef directory, kotatsu::string_list_ref arguments) {
        return normalizer.normalize(Spelling::absolute(directory), Fid{}, arguments);
    };
    auto spelled = *intern("/ws", split_command("clang++ -std=c++20 -Iinclude"));
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
    ZEXPECT(ranges::contains(rendered, "-std=c++20"sv));
    ZEXPECT(has_arg(rendered, "/ws/include"));
    ZEXPECT(std::string_view(rendered.back()) == file_table.resolve(ref.file).str());
};

ZEST_CASE(LeftOutOptionsShare) {
    /// What only the build reads — codegen options, outputs, dependency
    /// files, timings, listings — leaves the command: entries differing in
    /// it share one.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cpp", "clang++ -std=c++20 -fPIC -c a.cpp"sv);
    database.add_command("/fake",
                         "b.cpp",
                         "clang++ -std=c++20 -fPIC -g -fno-omit-frame-pointer -ffunction-sections "
                         "-c -o b.o -MD -MF b.d -MJ b.o.json -ftime-trace=b.trace.json "
                         "-save-temps=obj --serialize-diagnostics b.dia -Wa,-adhlns=b.lst "
                         "-frandom-seed=b.o b.cpp"sv);
    database.add_command("/fake", "c.cpp", "cl /std:c++20 /c /Foc.obj c.cpp"sv);
    database.add_command("/fake",
                         "d.cpp",
                         "cl /std:c++20 /c /Fod.obj /Fdd.pdb /Fad.asm /FRd.sbr /Fed.exe "
                         "/sourceDependencies d.json /showIncludes /showFilenames d.cpp"sv);
    auto command = [&](llvm::StringRef file) {
        return database.candidate_entries(fake(file)).front().command;
    };
    ZEXPECT(command("a.cpp") == command("b.cpp"));
    ZEXPECT(print_argv(commands.render_full(command("b.cpp"))) == "clang++ -std=c++20 -fPIC");
    ZEXPECT(command("c.cpp") == command("d.cpp"));
};

ZEST_CASE(ModuleEmbeddingDiscarded) {
    /// A module embedding its sources, as Bazel's module rules ask cc1 to,
    /// would override them in its importers: clice builds its modules from
    /// the files on disk, whichever spelling asks.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake",
                         "a.cppm",
                         "clang++ -std=c++20 -Xclang -fmodules-embed-all-files a.cppm"sv);
    database.add_command("/fake",
                         "b.cppm",
                         "clang++ -std=c++20 -fmodules-embed-all-files b.cppm"sv);
    for(auto file: {"a.cppm", "b.cppm"}) {
        auto argv = render_entry(database, fake(file));
        ZASSERT(argv.size() == 3U);
        ZEXPECT(argv[1] == "-std=c++20"sv);
    }
};

};  // ZEST_SUITE(Normalize)

}  // namespace

}  // namespace clice::testing
