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

ZEST_SUITE(Edit) {

ZEST_CASE(AppendBeforeSlot) {
    /// Appends go before the input slot, so an appended selector governs
    /// the input.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.c", "clang -x c a.c -x none"sv);

    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Append, {"-x", "c++"}}
    };
    auto applied = apply_edits(commands,
                               file_table,
                               database.candidate_entries(fake("a.c")).front().command,
                               {.edits = edits});
    ZEXPECT(llvm::StringRef(commands.input_kind(applied, "a.c").value) == "c++");
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
    /// spelling, and the command hash covers that spelling.
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

ZEST_CASE(RuleLeavesClSpellings) {
    /// A cl spelling the command's parse read stays clang's to spell once a
    /// rule takes the command out of cl mode.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    database.add_command("/fake", "a.cpp", "clang++ --driver-mode=cl /DFOO a.cpp"sv);
    auto& entry = database.candidate_entries(fake("a.cpp")).front();
    std::vector<CommandEdit> removed = {
        {.kind = CommandEdit::Kind::Remove, .flags = {"--driver-mode=cl"}}
    };
    CommandRef ref{entry.file,
                   apply_edits(commands, file_table, entry.command, {.edits = removed}),
                   commands.input_kind(entry.command, "a.cpp"),
                   CommandSource::CDBExact};
    auto argv = commands.render_driver(ref, database.spelling(ref.file));
    ZEXPECT(std::ranges::contains(argv, "-D"sv) || std::ranges::contains(argv, "-DFOO"sv));
    ZEXPECT(!std::ranges::contains(argv, "/D"sv));
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
    ZEXPECT(llvm::StringRef(render("/one"))
                .contains(Spelling("inc", Spelling::absolute("/one")).str()));
    ZEXPECT(llvm::StringRef(render("/two"))
                .contains(Spelling("inc", Spelling::absolute("/two")).str()));

    auto one = edits_at("/one");
    auto two = edits_at("/two");
    ZEXPECT(CommandEdits{.edits = one}.key() != CommandEdits{.edits = two}.key());
};

};  // ZEST_SUITE(Edit)

}  // namespace

}  // namespace clice::testing
