module clice;

import :command.table;
import :project.command;
import :project.project;
import :syntax.lexer;
import :unittests.utils;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;

namespace clice::testing {
namespace {

/// The file's resolution once the include trees its host search misses are
/// taken.
Resolution resolve_taking_trees(Project& project, Fid file, CommandRequest request = {}) {
    auto [resolution] = async::run(project.resolve_with_trees(file, [&] { return request; }));
    return std::move(*resolution);
}

ZEST_SUITE(ResolveCommand) {

ZEST_CASE(DefaultSourceKeepsCommand) {
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
    project.settings.rules.push_back(
        Rule{.patterns = {"src/**"}, .default_command = std::string("clang++ -DDEFAULTED")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.rules = Rules(project.settings, project.root, "");

    auto main = project.file_table.intern(Spelling::absolute(tmp.path("src/main.cpp")));
    auto part = project.file_table.intern(Spelling::absolute(tmp.path("src/part.cpp")));
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("src/part.h")));
    project.dep_graph.set_includes(main, 0, {{part}, {header}});
    project.dep_graph.build_reverse_map();

    auto part_resolution = project.resolve_command(part);
    ZEXPECT(part_resolution.ref.source == CommandSource::Default);
    ZEXPECT(ranges::any_of(part_resolution.arguments,
                           [](llvm::StringRef arg) { return arg.contains("DEFAULTED"); }));
    auto header_resolution = resolve_taking_trees(project, header);
    ZEXPECT(header_resolution.ref.source == CommandSource::Host);
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
    project.settings.rules.push_back(
        Rule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.rules = Rules(project.settings, project.root, "");

    auto main = project.file_table.intern(Spelling::absolute(tmp.path("src/main.cpp")));
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("src/part.h")));
    project.dep_graph.set_includes(main, 0, {{header}});
    project.dep_graph.build_reverse_map();

    std::vector<std::string> extras{"-DLINT_EXTRA"};
    CommandRequest request{.extra_append = extras};
    auto has_extra = [](llvm::ArrayRef<std::string> arguments) {
        return ranges::any_of(arguments,
                              [](llvm::StringRef arg) { return arg.contains("LINT_EXTRA"); });
    };
    auto main_resolution = resolve_taking_trees(project, main, request);
    ZEXPECT(main_resolution.ref.source == CommandSource::Default);
    ZEXPECT(has_extra(main_resolution.arguments));
    auto header_resolution = resolve_taking_trees(project, header, request);
    ZEXPECT(header_resolution.ref.source == CommandSource::Host);
    ZEXPECT(has_extra(header_resolution.arguments));
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
    project.settings.rules.push_back(Rule{.compile_commands = {"build"}});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.rules = Rules(project.settings, project.root, "");
    for(auto source: project.rules.declared_databases()) {
        project.cdb.load(source);
    }
    project.rebuild_dependency_graph();

    auto header = project.file_table.intern(Spelling::absolute(tmp.path("config_b/config.h")));
    auto a = project.file_table.intern(Spelling::absolute(tmp.path("a.cpp")));
    auto b = project.file_table.intern(Spelling::absolute(tmp.path("b.cpp")));
    auto waiting = project.resolve_command(header);
    ZASSERT(waiting.missing_tree);
    ZEXPECT(waiting.missing_tree->key.first == a);
    // Meanwhile the header stands on a.cpp's command, a.cpp its host.
    ZEXPECT(waiting.host == a);
    ZEXPECT(waiting.ref.source == CommandSource::Host);
    ZEXPECT(ranges::any_of(waiting.arguments,
                           [](llvm::StringRef argument) { return argument.contains("config_a"); }));
    auto resolution = resolve_taking_trees(project, header);
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
    project.file_table.intern(Spelling::absolute(tmp.path("later.h")));
    project.settings.rules.push_back(Rule{.compile_commands = {"build"}});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.rules = Rules(project.settings, project.root, "");
    for(auto source: project.rules.declared_databases()) {
        project.cdb.load(source);
    }
    project.rebuild_dependency_graph();

    auto forced = [&](Fid file) {
        auto resolution = resolve_taking_trees(project, file);
        auto& arguments = resolution.arguments;
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

    // Its self-containment check found it needs its includer's context.
    auto key = forced(part).first.self_containment;
    ZASSERT(key);
    auto version = current_input(project.file_table, part)->version;
    project.hosting.self_containment.commit(
        *key,
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

ZEST_CASE(DialectFollowsCommand) {
    /// A file is lexed in the dialect of the command it compiles under: a
    /// header in the language and standard of the host it compiles in, the
    /// user's chosen host's when there is one, and a file borrowing a
    /// command in that command's standard.
    TempDir tmp;
    tmp.touch("h.c", "#include \"h.h\"\n");
    tmp.touch("other.cpp", "#include \"h.h\"\n");
    tmp.touch("h.h", "");
    tmp.touch("alone/x.cpp", "");
    tmp.touch("alone/x.h", "");
    FileTable files;
    Project project{files};
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.rules = Rules(project.settings, project.root, "");
    auto add = [&](llvm::StringRef command, llvm::StringRef file) {
        auto line = std::format("{} -c {}", command, tmp.path(file));
        return project.cdb.add_command(tmp.root, tmp.path(file), llvm::StringRef(line))->file;
    };
    auto c_host = add("clang -std=c11", "h.c");
    auto cxx_host = add("clang++ -std=c++17", "other.cpp");
    add("clang++ -std=c++14", "alone/x.cpp");
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("h.h")));
    auto borrowing = project.file_table.intern(Spelling::absolute(tmp.path("alone/x.h")));
    project.dep_graph.set_includes(c_host, 0, {{header}});
    project.dep_graph.set_includes(cxx_host, 0, {{header}});
    project.dep_graph.build_reverse_map();

    ZASSERT(project.select_command(header).host == c_host);
    auto& in_c = project.dialect(header, nullptr);
    ZEXPECT(!bool(in_c.CPlusPlus));
    ZEXPECT((in_c.LangStd == clang::LangStandard::lang_c11));

    ContextChoice choice{.host = cxx_host};
    auto& in_cxx = project.dialect(header, &choice);
    ZEXPECT(bool(in_cxx.CPlusPlus));
    ZEXPECT((in_cxx.LangStd == clang::LangStandard::lang_cxx17));

    ZASSERT(project.select_command(borrowing).ref.source == CommandSource::Borrowed);
    auto& borrowed = project.dialect(borrowing, nullptr);
    ZEXPECT(bool(borrowed.CPlusPlus));
    ZEXPECT((borrowed.LangStd == clang::LangStandard::lang_cxx14));
}

ZEST_CASE(DialectOfLanguage) {
    /// The master lexes a file in the language its command compiles it in,
    /// as the frontend does: Objective-C, Objective-C++ and OpenCL C read
    /// their own keywords.
    TempDir tmp;
    tmp.touch("shape.m", "");
    tmp.touch("shape.mm", "");
    tmp.touch("kernel.cl", "");
    tmp.touch("kernel.clcpp", "");
    FileTable files;
    Project project{files};
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.rules = Rules(project.settings, project.root, "");
    auto add = [&](llvm::StringRef command, llvm::StringRef file) {
        auto line = std::format("{} -c {}", command, tmp.path(file));
        return project.cdb.add_command(tmp.root, tmp.path(file), llvm::StringRef(line))->file;
    };
    auto& objc = project.dialect(add("clang -std=c11", "shape.m"), nullptr);
    ZEXPECT(bool(objc.ObjC));
    ZEXPECT(!bool(objc.CPlusPlus));
    ZEXPECT((objc.LangStd == clang::LangStandard::lang_c11));
    auto& objcxx = project.dialect(add("clang++", "shape.mm"), nullptr);
    ZEXPECT(bool(objcxx.ObjC));
    ZEXPECT(bool(objcxx.CPlusPlus));
    auto& opencl = project.dialect(add("clang -cl-std=CL3.0", "kernel.cl"), nullptr);
    ZEXPECT(bool(opencl.OpenCL));
    ZEXPECT((opencl.LangStd == clang::LangStandard::lang_opencl30));
    ZEXPECT(keyword_kind("__global", keywords(opencl)) != clang::tok::identifier);
    auto& openclcxx = project.dialect(add("clang -cl-std=clc++2021", "kernel.clcpp"), nullptr);
    ZEXPECT(bool(openclcxx.OpenCLCPlusPlus));
    ZEXPECT((openclcxx.LangStd == clang::LangStandard::lang_openclcpp2021));
}

ZEST_CASE(IdentityIsEffectiveCommand) {
    /// A unit's index is keyed by the command it compiles under, rules
    /// applied, with the project root written as `${workspace}`: a checkout
    /// moved elsewhere keeps it, a rule whose configuration moves under
    /// .clice/ — its relative paths mean another directory — changes it.
    auto identity = [](TempDir& tmp, llvm::StringRef rule_directory) {
        tmp.touch("src/main.cpp", "");
        FileTable files;
        Project project{files};
        project.settings.rules.push_back(Rule{
            .patterns = {"${workspace}/src/**"},
            .default_command = std::string("clang++"),
            .append = {"-Iinc"},
        });
        project.settings.rules.back().directory =
            rule_directory.empty() ? tmp.root : tmp.path(rule_directory);
        project.root = CanonicalPath(Spelling::absolute(tmp.root));
        project.commands.set_project_root(project.root);
        project.settings.finalize(project.root);
        project.rules = Rules(project.settings, project.root, "");
        auto main = project.file_table.intern(Spelling::absolute(tmp.path("src/main.cpp")));
        return project.command_identity(main);
    };
    TempDir checkout;
    TempDir moved;
    auto at_root = identity(checkout, "");
    ZEXPECT(at_root != 0u);
    ZEXPECT(identity(moved, "") == at_root);
    auto under_clice = identity(checkout, ".clice");
    ZEXPECT(under_clice != 0u);
    ZEXPECT(under_clice != at_root);
}

};  // ZEST_SUITE(ResolveCommand)

}  // namespace
}  // namespace clice::testing
