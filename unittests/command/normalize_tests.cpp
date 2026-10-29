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
    /// its own options.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cpp", "ccache clang++ -std=c++20 a.cpp"sv);
    database.add_command("/fake", "b.cpp", "clang++ -std=c++20 b.cpp"sv);
    database.add_command("/fake",
                         "c.cpp",
                         "ccache --set-config cache_dir=/tmp/cc clang++ -std=c++20 c.cpp"sv);

    auto& a = database.candidate_entries(fake("a.cpp")).front();
    auto& b = database.candidate_entries(fake("b.cpp")).front();
    auto& c = database.candidate_entries(fake("c.cpp")).front();
    ZEXPECT(a.command == b.command);
    ZEXPECT(c.command == b.command);
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
    ZEXPECT(ranges::contains(rendered, "-std=c++20"sv));
    ZEXPECT(has_arg(rendered, "/ws/include"));
    ZEXPECT(std::string_view(rendered.back()) == file_table.resolve(ref.file).str());
};

ZEST_CASE(CodegenKeptInFull) {
    /// Codegen-only options, which the compile render leaves out, survive
    /// in the full render.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command(
        "/fake",
        "main.cpp",
        "clang++ -std=c++20 -fPIC -fno-omit-frame-pointer -fstack-protector-strong "
        "-fdata-sections -ffunction-sections -fcolor-diagnostics -g main.cpp"sv);

    auto full = print_argv(
        commands.render_full(database.candidate_entries(fake("main.cpp")).front().command));
    ZEXPECT(llvm::StringRef(full).contains("-fno-omit-frame-pointer"));
    ZEXPECT(llvm::StringRef(full).contains("-fdata-sections"));
};

};  // ZEST_SUITE(Normalize)

}  // namespace

}  // namespace clice::testing
