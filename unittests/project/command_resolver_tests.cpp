module clice;

import :command.table;
import :project.command_resolver;
import :project.hosting;
import :project.project;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(CommandResolver) {

ZEST_CASE(DefaultSourceKeepsOwnCommand) {
    /// A unity build under a default command: main.cpp includes part.cpp
    /// and part.h. The included source is a unit of its own and keeps the
    /// default command the index compiles it with; the header borrows
    /// main.cpp's.
    TempDir tmp;
    tmp.touch("src/main.cpp", R"(#include "part.cpp"
#include "part.h")");
    tmp.touch("src/part.cpp", "");
    tmp.touch("src/part.h", "");
    FileTable files;
    Project project{files};
    CommandResolver resolver(project);
    project.settings.rules.push_back(
        Rule{.patterns = {"src/**"}, .default_command = std::string("clang++ -DDEFAULTED")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.build.reset_active("");

    auto main = project.file_table.intern(Spelling::absolute(tmp.path("src/main.cpp")));
    auto part = project.file_table.intern(Spelling::absolute(tmp.path("src/part.cpp")));
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("src/part.h")));
    project.dep_graph.set_includes(main, 0, {{part}, {header}});
    project.dep_graph.build_reverse_map();

    std::string directory;
    std::vector<std::string> arguments;
    ZEXPECT(resolver.resolve_command(part, directory, arguments).source == CommandSource::Default);
    ZEXPECT(
        ranges::any_of(arguments, [](llvm::StringRef arg) { return arg.contains("DEFAULTED"); }));
    auto header_resolution =
        resolver.resolve_command(header, directory, arguments, {.preprocess = true});
    ZEXPECT(header_resolution.source == CommandSource::IncludeGraph);
    ZEXPECT(header_resolution.host == main);
}

ZEST_CASE(ExtrasReachBorrowedHeader) {
    /// A lint run's extra args join the command a header borrows from its
    /// host, as they join the host's own.
    TempDir tmp;
    tmp.touch("src/main.cpp", R"(#include "part.h")");
    tmp.touch("src/part.h", "");
    FileTable files;
    Project project{files};
    CommandResolver resolver(project);
    project.settings.rules.push_back(
        Rule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.build.reset_active("");

    auto main = project.file_table.intern(Spelling::absolute(tmp.path("src/main.cpp")));
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("src/part.h")));
    project.dep_graph.set_includes(main, 0, {{header}});
    project.dep_graph.build_reverse_map();

    std::vector<std::string> extras{"-DLINT_EXTRA"};
    CommandRequest request{.extra_append = extras, .preprocess = true};
    auto has_extra = [](llvm::ArrayRef<std::string> arguments) {
        return ranges::any_of(arguments,
                              [](llvm::StringRef arg) { return arg.contains("LINT_EXTRA"); });
    };
    std::string directory;
    std::vector<std::string> arguments;
    ZEXPECT(resolver.resolve_command(main, directory, arguments, request).source ==
            CommandSource::Default);
    ZEXPECT(has_extra(arguments));
    auto header_resolution = resolver.resolve_command(header, directory, arguments, request);
    ZEXPECT(header_resolution.source == CommandSource::IncludeGraph);
    ZEXPECT(has_extra(arguments));
}

ZEST_CASE(TreeSkipsFirstHost) {
    /// The scan resolved shared.h's include under b.cpp's directories;
    /// a.cpp ranks first, but under its directories the include reaches
    /// another config.h. The search waits for a.cpp's include tree, which
    /// rules it out, and b.cpp's compile enters the header.
    TempDir tmp;
    tmp.touch("common/shared.h", "#pragma once\n#include <config.h>\n");
    tmp.touch("config_a/config.h", "#pragma once\n");
    tmp.touch("config_b/config.h", "#pragma once\n");
    tmp.touch("a.cpp", "#include \"shared.h\"\n");
    tmp.touch("b.cpp", "#include \"shared.h\"\n");
    tmp.touch("build/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("b.cpp"), {"-Iconfig_b", "-Icommon"}},
                  {tmp.root, tmp.path("a.cpp"), {"-Iconfig_a", "-Icommon"}},
    }));
    FileTable files;
    Project project{files};
    CommandResolver resolver(project);
    project.settings.rules.push_back(Rule{.compile_commands = {"build"}});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.build.reset_active("");
    for(auto source: project.build.declared_sources()) {
        project.cdb.load(source);
    }
    project.rebuild_dependency_graph();

    auto header = project.file_table.intern(Spelling::absolute(tmp.path("config_b/config.h")));
    auto a = project.file_table.intern(Spelling::absolute(tmp.path("a.cpp")));
    auto b = project.file_table.intern(Spelling::absolute(tmp.path("b.cpp")));
    std::string directory;
    std::vector<std::string> arguments;
    auto waiting = resolver.resolve_command(header, directory, arguments);
    ZASSERT(waiting.missing_tree);
    ZEXPECT(waiting.missing_tree->key.first == a);
    ZEXPECT(!waiting.host.valid());
    // Meanwhile the header stands on a.cpp's command.
    ZEXPECT(waiting.source == CommandSource::IncludeGraph);
    ZEXPECT(ranges::any_of(arguments,
                           [](llvm::StringRef argument) { return argument.contains("config_a"); }));
    auto resolution = resolver.resolve_command(header, directory, arguments, {.preprocess = true});
    ZEXPECT(!resolution.missing_tree);
    ZEXPECT(resolution.host == b);
}

ZEST_CASE(ForcedChainDropsIncludes) {
    /// main.cpp's command forces in config.h, pch.h, later.h and config.h
    /// again; pch.h and later.h both include part.h, which part.h's compile
    /// enters through pch.h, the first. Its command keeps what comes before
    /// pch.h alone; a context replaces pch.h by its text before the
    /// include. pch.h itself keeps config.h alone. later.h is named before
    /// the scan, so the order the command forces them in decides, not the
    /// order files were named in.
    TempDir tmp;
    tmp.touch("config.h", "#pragma once\n");
    tmp.touch("pch.h", R"(#pragma once
#define FROM_PCH 1
#include "part.h"
)");
    tmp.touch("later.h", R"(#pragma once
#include "part.h"
)");
    tmp.touch("part.h", "#pragma once\n");
    tmp.touch("main.cpp", R"(#include "part.h")");
    tmp.touch("build/compile_commands.json",
              build_cdb_json({
                  {tmp.root,
                   tmp.path("main.cpp"),
                   {"-include",
                    "config.h",
                    "-include",
                    "pch.h",
                    "-include",
                    "later.h",
                    "-include",
                    "config.h"}},
    }));
    FileTable files;
    Project project{files};
    CommandResolver resolver(project);
    project.file_table.intern(Spelling::absolute(tmp.path("later.h")));
    project.settings.rules.push_back(Rule{.compile_commands = {"build"}});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.build.reset_active("");
    for(auto source: project.build.declared_sources()) {
        project.cdb.load(source);
    }
    project.rebuild_dependency_graph();

    auto forced = [&](Fid file) {
        std::string directory;
        std::vector<std::string> arguments;
        auto resolution =
            resolver.resolve_command(file, directory, arguments, {.preprocess = true});
        std::vector<std::string> names;
        for(std::size_t i = 0; i + 1 < arguments.size(); i += 1) {
            if(arguments[i] == "-include") {
                names.push_back(arguments[i + 1]);
            }
        }
        return std::pair(std::move(resolution), names);
    };
    auto main = project.file_table.intern(Spelling::absolute(tmp.path("main.cpp")));
    auto pch = project.file_table.intern(Spelling::absolute(tmp.path("pch.h")));
    auto part = project.file_table.intern(Spelling::absolute(tmp.path("part.h")));
    ZEXPECT(forced(main).second ==
            std::vector<std::string>{"config.h", "pch.h", "later.h", "config.h"});
    ZEXPECT(forced(part).second == std::vector<std::string>{"config.h"});
    ZEXPECT(forced(pch).second == std::vector<std::string>{"config.h"});

    // Its trial found it needs its includer's context.
    auto trial = forced(part).first.trial;
    ZASSERT(trial);
    auto version = current_dep(project.file_table, part)->version;
    project.hosting.trials.commit(*trial,
                                  {.self_contained = false, .text = version, .disk = version},
                                  {});
    auto [resolution, names] = forced(part);
    ZASSERT(resolution.synthesized);
    ZEXPECT(resolution.forced == pch);
    ZASSERT(names.size() == 2u);
    ZEXPECT(names[0] == "config.h");
    ZEXPECT(names[1] == resolution.synthesized->prefix);
    ZEXPECT(ranges::any_of(resolution.synthesized->files, [](const SynthesizedFile& file) {
        return llvm::StringRef(file.content).contains("FROM_PCH");
    }));
}

};  // ZEST_SUITE(CommandResolver)

}  // namespace
}  // namespace clice::testing
