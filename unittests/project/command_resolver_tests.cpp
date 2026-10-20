module;

#include "modules/prelude.h"

module clice;

import :command.argument_parser;
import :project.command_resolver;
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
    project.config.rules.push_back(
        ConfigRule{.patterns = {"src/**"}, .default_command = std::string("clang++ -DDEFAULTED")});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");

    auto main = project.file_table.intern(Spelling::absolute(tmp.path("src/main.cpp")));
    auto part = project.file_table.intern(Spelling::absolute(tmp.path("src/part.cpp")));
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("src/part.h")));
    project.dep_graph.set_includes(main, 0, {{part}, {header}});
    project.dep_graph.build_reverse_map();

    std::string directory;
    std::vector<std::string> arguments;
    ZEXPECT(resolver.resolve_command(part, directory, arguments).source == CommandSource::Default);
    ZEXPECT(llvm::any_of(arguments, [](llvm::StringRef arg) { return arg.contains("DEFAULTED"); }));
    auto header_resolution = resolver.resolve_command(header, directory, arguments);
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
    project.config.rules.push_back(
        ConfigRule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");

    auto main = project.file_table.intern(Spelling::absolute(tmp.path("src/main.cpp")));
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("src/part.h")));
    project.dep_graph.set_includes(main, 0, {{header}});
    project.dep_graph.build_reverse_map();

    std::vector<std::string> extras{"-DLINT_EXTRA"};
    CommandRequest request{.extra_append = extras};
    auto has_extra = [](llvm::ArrayRef<std::string> arguments) {
        return llvm::any_of(arguments,
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

ZEST_CASE(UnboundModeStaysLocal) {
    // A NeedsContext result scored with no disk observation has no hash
    // to validate on load: it serves this session but must neither
    // persist nor, if found in a blob, bypass the content gate — the next
    // session's bytes never earned it.
    TempDir tmp;
    FileTable files;
    Project project{files};
    CommandResolver resolver(project);
    tmp.touch("h.h", "int x;\n");
    auto path = tmp.path("h.h");
    auto id = project.file_table.intern(Spelling::absolute(path));

    resolver.record_header_mode(id, HeaderMode::NeedsContext);
    ZASSERT(resolver.header_mode(id) == HeaderMode::NeedsContext);

    std::vector<ContextHeaderEntry> headers;
    resolver.dump_context_headers(headers, [](Fid fid) { return fid.raw; });
    ZASSERT(headers.empty());

    CommandResolver restarted(project);
    headers.push_back({id.raw, 0});
    restarted.load_context_headers(headers,
                                   [&](std::uint32_t) -> std::optional<Fid> { return id; });
    ZASSERT(restarted.header_mode(id) == HeaderMode::Unknown);
}

ZEST_CASE(ContextHeaderContentGate) {
    // A content-bound result survives a restart only while the disk
    // still holds the bytes it was scored on.
    TempDir tmp;
    FileTable files;
    Project project{files};
    CommandResolver resolver(project);
    tmp.touch("h.h", "int x;\n");
    auto path = tmp.path("h.h");
    auto id = project.file_table.intern(Spelling::absolute(path));
    auto disk = project.file_table.disk.current(id);
    ZASSERT(disk);

    resolver.record_header_mode(id, HeaderMode::NeedsContext, disk->hash);
    std::vector<ContextHeaderEntry> headers;
    resolver.dump_context_headers(headers, [](Fid fid) { return fid.raw; });
    ZASSERT(headers.size() == 1u);

    auto resolve = [&](std::uint32_t) -> std::optional<Fid> {
        return id;
    };
    CommandResolver same_disk(project);
    same_disk.load_context_headers(headers, resolve);
    ZASSERT(same_disk.header_mode(id) == HeaderMode::NeedsContext);

    tmp.touch("h.h", "int y;\n");
    CommandResolver edited(project);
    edited.load_context_headers(headers, resolve);
    ZASSERT(edited.header_mode(id) == HeaderMode::Unknown);
}

ZEST_CASE(ModePersistenceMarksDirty) {
    // The persisted context headers and the artifact records move together: any
    // transition of a content-bound NeedsContext — earned, downgraded by
    // a trial, or reset by a dependency change — must rewrite the blob,
    // or a restart resurrects the dropped result (the header's own hash
    // still matches). Session-local transitions must not thrash it.
    FileTable files;
    Project project{files};
    CommandResolver resolver(project);
    auto id = project.file_table.intern(Spelling::absolute("/proj/h.h"));

    resolver.record_header_mode(id, HeaderMode::NeedsContext, 7);
    ZASSERT(project.artifacts_dirty);

    project.artifacts_dirty = false;
    resolver.reset_header_mode(id);
    ZASSERT(project.artifacts_dirty);

    // Unbound results and self-contained impressions are never persisted.
    project.artifacts_dirty = false;
    resolver.record_header_mode(id, HeaderMode::NeedsContext);
    resolver.record_header_mode(id, HeaderMode::SelfContained);
    resolver.reset_header_mode(id);
    ZASSERT(!project.artifacts_dirty);

    // A trial downgrading a persisted result drops it from the blob.
    resolver.record_header_mode(id, HeaderMode::NeedsContext, 7);
    project.artifacts_dirty = false;
    resolver.record_header_mode(id, HeaderMode::SelfContained);
    ZASSERT(project.artifacts_dirty);
}

ZEST_CASE(SelfContainedFlipBumpsEpoch) {
    // A self-contained header's listing merges hosts the others list one
    // by one: entering or leaving that result changes the listing, and
    // nothing else does.
    FileTable files;
    Project project{files};
    CommandResolver resolver(project);
    auto id = project.file_table.intern(Spelling::absolute("/proj/h.h"));
    auto epoch = project.context_epoch;

    resolver.record_header_mode(id, HeaderMode::NeedsContext);
    ZEXPECT(project.context_epoch == epoch);
    resolver.record_header_mode(id, HeaderMode::SelfContained);
    ZEXPECT(project.context_epoch == epoch + 1);
    resolver.record_header_mode(id, HeaderMode::SelfContained);
    ZEXPECT(project.context_epoch == epoch + 1);
    resolver.forget_self_contained(id);
    ZEXPECT(project.context_epoch == epoch + 2);
    resolver.forget_self_contained(id);
    ZEXPECT(project.context_epoch == epoch + 2);

    resolver.record_header_mode(id, HeaderMode::SelfContained);
    resolver.reset_header_mode(id);
    ZEXPECT(project.context_epoch == epoch + 4);
    resolver.reset_header_mode(id);
    ZEXPECT(project.context_epoch == epoch + 4);
}

ZEST_CASE(UnmatchedChainWantsTree) {
    /// The scan resolved shared.h's include under b.cpp's directories;
    /// a.cpp ranks first, but under its directories the include reaches
    /// another config.h. The resolution names the host whose chain it
    /// could not follow, and asks for that host's include tree.
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
    project.config.rules.push_back(ConfigRule{.compile_commands = {"build"}});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");
    for(auto source: project.build.declared_sources()) {
        project.cdb.load(source);
    }
    project.rebuild_dependency_graph();

    auto header = project.file_table.intern(Spelling::absolute(tmp.path("config_b/config.h")));
    auto a = project.file_table.intern(Spelling::absolute(tmp.path("a.cpp")));
    resolver.record_header_mode(header, HeaderMode::NeedsContext);
    std::string directory;
    std::vector<std::string> arguments;
    auto resolution = resolver.resolve_command(header, directory, arguments);
    ZEXPECT(resolution.unmatched_host == a);
    ZEXPECT(resolution.tree_wanted == a);
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
    project.config.rules.push_back(ConfigRule{.compile_commands = {"build"}});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");
    for(auto source: project.build.declared_sources()) {
        project.cdb.load(source);
    }
    project.rebuild_dependency_graph();

    auto forced = [&](Fid file) {
        std::string directory;
        std::vector<std::string> arguments;
        auto resolution = resolver.resolve_command(file, directory, arguments);
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

    resolver.record_header_mode(part, HeaderMode::NeedsContext);
    auto [resolution, names] = forced(part);
    ZASSERT(resolution.synthesized);
    ZEXPECT(resolution.forced == pch);
    ZASSERT(names.size() == 2u);
    ZEXPECT(names[0] == "config.h");
    ZEXPECT(names[1] == resolution.synthesized->prefix);
    ZEXPECT(llvm::any_of(resolution.synthesized->files, [](const SynthesizedFile& file) {
        return llvm::StringRef(file.content).contains("FROM_PCH");
    }));
}

};  // ZEST_SUITE(CommandResolver)

}  // namespace
}  // namespace clice::testing
