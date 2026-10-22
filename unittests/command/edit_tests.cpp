module;

#include "modules/prelude.h"

module clice;

import :command.argument;
import :command.database;
import :command.edit;
import :command.table;
import :support.shell;
import :unittests.utils.cdb_helper;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {

namespace {

using namespace std::literals;

#define EXPECT_CONTAINS(haystack, needle) ZEXPECT(llvm::StringRef(haystack).contains(needle))
#define EXPECT_NOT_CONTAINS(haystack, needle) ZEXPECT(!llvm::StringRef(haystack).contains(needle))

ZEST_SUITE(Edit) {

ZEST_CASE(RemoveAppend) {
    llvm::SmallVector args = {
        "clang++",
        "--output=main.o",
        "-D",
        "A",
        "-D",
        "B=0",
        "main.cpp",
    };

    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "main.cpp", args);
    auto main = fake("main.cpp");

    std::vector<CommandEdit> edits;
    CommandEdits options{.edits = edits};
    auto remove_only = [&](std::vector<std::string> flags) {
        edits = {
            {CommandEdit::Kind::Remove, std::move(flags)}
        };
        options.edits = edits;
    };

    remove_only({"-DA"});
    ZEXPECT(print_argv(render_entry(database, main, options)) ==
            std::format("clang++ -D B=0 {}", main));

    remove_only({"-D", "A"});
    ZEXPECT(print_argv(render_entry(database, main, options)) ==
            std::format("clang++ -D B=0 {}", main));

    remove_only({"-DA", "-D", "B=0"});
    ZEXPECT(print_argv(render_entry(database, main, options)) == std::format("clang++ {}", main));

    remove_only({"-D*"});
    ZEXPECT(print_argv(render_entry(database, main, options)) == std::format("clang++ {}", main));

    remove_only({"-D", "*"});
    ZEXPECT(print_argv(render_entry(database, main, options)) == std::format("clang++ {}", main));

    edits = {
        {CommandEdit::Kind::Append, {"-D", "C"}}
    };
    options.edits = edits;
    ZEXPECT(print_argv(render_entry(database, main, options)) ==
            std::format("clang++ -D A -D B=0 -D C {}", main));
};

ZEST_CASE(AppendUnknownValue) {
    /// An appended option the table does not know keeps its separate value:
    /// an edit cannot name the entry input, so an input-classified token is
    /// really the option's value.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "main.cpp", "clang++ main.cpp"sv);

    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Append, {"-fnot-a-real-flag", "value"}}
    };
    CommandEdits options{.edits = edits};
    ZEXPECT(print_argv(render_entry(database, fake("main.cpp"), options)) ==
            std::format("clang++ -fnot-a-real-flag value {}", fake("main.cpp")));
};

ZEST_CASE(AppendBeforeSlot) {
    /// Appends go before the input slot, so an appended selector governs
    /// the input; the CDB's selector after it still governs none.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.c", "clang -x c a.c -x none"sv);

    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Append, {"-x", "c++"}}
    };
    CommandEdits options{.edits = edits};

    ZEXPECT(print_argv(render_entry(database, fake("a.c"), options)) ==
            std::format("clang -x c -x c++ {} -x none", fake("a.c")));

    auto applied = apply_edits(commands,
                               file_table,
                               database.candidate_entries(fake("a.c")).front().command,
                               options);
    ZEXPECT(llvm::StringRef(commands.input_kind(applied, "a.c").value) == "c++");
};

ZEST_CASE(AppendAfterInputFlags) {
    /// An append overrides a flag the CDB writes after its input.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.c", "clang -c a.c -std=c11"sv);

    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Append, {"-std=c17"}}
    };
    CommandEdits options{.edits = edits};
    ZEXPECT(print_argv(render_entry(database, fake("a.c"), options)) ==
            std::format("clang -std=c11 -std=c17 {}", fake("a.c")));
};

ZEST_CASE(RemoveEveryValue) {
    /// A flag written without its value, last in the list, or with `*` for
    /// it, removes every value; a `*` joined to the name removes every flag
    /// of that prefix and kind, so the preprocessor's -Wp stays. One that
    /// takes several values removes nothing until they are written.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command(
        "/fake",
        "a.c",
        "clang -include pre.h -Iinc -Wall -Wextra -Wno-unused -Wp,-DKEPT -DKEEP a.c"sv);

    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Remove, {"-I*", "-W*", "-include"}},
        {CommandEdit::Kind::Remove, {"-sectcreate"}           },
    };
    ZEXPECT(print_argv(render_entry(database, fake("a.c"), {.edits = edits})) ==
            std::format("clang -Wp,-DKEPT -D KEEP {}", fake("a.c")));
};

ZEST_CASE(SelectorHistoryRestored) {
    /// Removing the later selector re-exposes the earlier one.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.c", "clang -x cuda -x c++ a.c"sv);

    auto base = database.candidate_entries(fake("a.c")).front().command;
    ZEXPECT(llvm::StringRef(commands.input_kind(base, "a.c").value) == "c++");

    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Remove, {"-x", "c++"}}
    };
    CommandEdits options{.edits = edits};
    auto applied = apply_edits(commands, file_table, base, options);
    ZEXPECT(llvm::StringRef(commands.input_kind(applied, "a.c").value) == "cuda");
};

ZEST_CASE(RuleUnknownFlagIdentity) {
    /// A flag a rule appends that the option table does not know keeps its
    /// spelling, and the identity hashes that spelling.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cpp", "clang++ -std=c++20 a.cpp"sv);
    auto base = database.candidate_entries(fake("a.cpp")).front().command;
    auto hash_with = [&](std::string flag) {
        std::vector<CommandEdit> edits = {
            {CommandEdit::Kind::Append, {std::move(flag)}}
        };
        return commands.command_hash(apply_edits(commands, file_table, base, {.edits = edits}));
    };

    ZEXPECT(hash_with("--clice-unknown-one") != hash_with("--clice-unknown-two"));
    ZEXPECT(hash_with("--clice-unknown-one") != commands.command_hash(base));
};

ZEST_CASE(RuleDriverModeFamily) {
    /// The family follows the driver mode a rule leaves, the last one
    /// winning as in clang.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cpp", "clang++ --driver-mode=cl a.cpp"sv);
    auto base = database.candidate_entries(fake("a.cpp")).front().command;
    ZEXPECT(commands.get(base).family == CompilerFamily::ClangCL);

    std::vector<CommandEdit> removed = {
        {CommandEdit::Kind::Remove, {"--driver-mode=cl"}}
    };
    ZEXPECT(commands.get(apply_edits(commands, file_table, base, {.edits = removed})).family !=
            CompilerFamily::ClangCL);
    std::vector<CommandEdit> overridden = {
        {CommandEdit::Kind::Append, {"--driver-mode=g++"}}
    };
    ZEXPECT(commands.get(apply_edits(commands, file_table, base, {.edits = overridden})).family !=
            CompilerFamily::ClangCL);
};

ZEST_CASE(PrependAfterBinary) {
    llvm::SmallVector args = {"clang++", "-DA", "main.cpp"};

    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "main.cpp", args);

    CommandEdits options;
    llvm::SmallVector<std::string> prepend = {"-std=c++17", "-DB"};
    options.extra_prepend = prepend;
    // Prepends sit ahead of the command's own flags, so the command wins
    // on collision. (Defines render canonicalized, as two tokens.)
    ZEXPECT(print_argv(render_entry(database, fake("main.cpp"), options)) ==
            std::format("clang++ -std=c++17 -D B -D A {}", fake("main.cpp")));
};

ZEST_CASE(LaterRemoveCancelsAppend) {
    /// Edits apply in rule order: a later remove reaches what an earlier
    /// rule appended, by option semantics (spelling-independent).
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "main.cpp", "clang++ main.cpp"sv);
    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Append, {"-DFOO=1", "-DBAR"}},
        {CommandEdit::Kind::Remove, {"-D", "FOO=1"}     },
    };
    CommandEdits options{.edits = edits};
    ZEXPECT(print_argv(render_entry(database, fake("main.cpp"), options)) ==
            std::format("clang++ -D BAR {}", fake("main.cpp")));

    /// The other way round the append wins: nothing before it to cancel.
    std::vector<CommandEdit> reversed = {
        {CommandEdit::Kind::Remove, {"-DFOO=1"}},
        {CommandEdit::Kind::Append, {"-DFOO=1"}},
    };
    options.edits = reversed;
    ZEXPECT(print_argv(render_entry(database, fake("main.cpp"), options)) ==
            std::format("clang++ -D FOO=1 {}", fake("main.cpp")));
};

ZEST_CASE(RemoveIncludeWildcard) {
    /// `*` removes every value of a path option instead of naming a file
    /// in the rule's directory.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "main.cpp", "clang++ -Iinc -I/opt/include main.cpp"sv);
    for(auto flags: {
            std::vector<std::string>{"-I*"},
            std::vector<std::string>{"-I", "*"}
    }) {
        std::vector<CommandEdit> edits = {
            {CommandEdit::Kind::Remove, flags, Spelling::absolute("/config")}
        };
        ZEXPECT(print_argv(render_entry(database, fake("main.cpp"), {.edits = edits})) ==
                std::format("clang++ {}", fake("main.cpp")));
    }
};

ZEST_CASE(RuleAnchorKeysMemo) {
    /// One rule text read from two configuration directories names two
    /// include directories.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "main.cpp", "clang++ main.cpp"sv);
    auto edits_at = [](llvm::StringRef directory) {
        return std::vector<CommandEdit>{
            {CommandEdit::Kind::Append, {"-Iinc"}, Spelling::absolute(directory)}
        };
    };
    auto render = [&](llvm::StringRef directory) {
        auto edits = edits_at(directory);
        return print_argv(render_entry(database, fake("main.cpp"), {.edits = edits}));
    };
    EXPECT_CONTAINS(render("/one"), Spelling("inc", Spelling::absolute("/one")).str());
    EXPECT_CONTAINS(render("/two"), Spelling("inc", Spelling::absolute("/two")).str());

    auto one = edits_at("/one");
    auto two = edits_at("/two");
    ZEXPECT(CommandEdits{.edits = one}.key() != CommandEdits{.edits = two}.key());
};

};  // ZEST_SUITE(Edit)

}  // namespace

}  // namespace clice::testing
