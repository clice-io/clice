#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "command/argument_parser.h"
#include "project/command_resolver.h"

namespace clice::testing {
namespace {

TEST_SUITE(CommandResolver) {

TEST_CASE(DefaultSourceKeepsOwnCommand) {
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
    EXPECT_EQ(resolver.resolve_command(tmp.path("src/part.cpp"), directory, arguments).source,
              CommandSource::Default);
    EXPECT_TRUE(
        llvm::any_of(arguments, [](llvm::StringRef arg) { return arg.contains("DEFAULTED"); }));
    auto header_resolution = resolver.resolve_command(tmp.path("src/part.h"), directory, arguments);
    EXPECT_EQ(header_resolution.source, CommandSource::IncludeGraph);
    EXPECT_EQ(header_resolution.host, main);
}

TEST_CASE(UnboundVerdictStaysLocal) {
    // A NeedsContext verdict scored with no disk observation has no hash
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
    ASSERT_TRUE(resolver.header_mode(id) == HeaderMode::NeedsContext);

    std::vector<CacheModeEntry> slices;
    resolver.dump_mode_slices(slices, [](Fid fid) { return fid.raw; });
    ASSERT_TRUE(slices.empty());

    CommandResolver restarted(project);
    slices.push_back({id.raw, static_cast<std::uint32_t>(HeaderMode::NeedsContext), 0});
    restarted.load_mode_slices(slices, [&](std::uint32_t) -> llvm::StringRef { return path; });
    ASSERT_TRUE(restarted.header_mode(id) == HeaderMode::Unknown);
}

TEST_CASE(ModeSliceContentGate) {
    // A content-bound verdict survives a restart only while the disk
    // still holds the bytes it was scored on.
    TempDir tmp;
    FileTable files;
    Project project{files};
    CommandResolver resolver(project);
    tmp.touch("h.h", "int x;\n");
    auto path = tmp.path("h.h");
    auto id = project.file_table.intern(Spelling::absolute(path));
    auto disk = project.file_table.current(id);
    ASSERT_TRUE(disk.has_value());

    resolver.record_header_mode(id, HeaderMode::NeedsContext, disk->hash);
    std::vector<CacheModeEntry> slices;
    resolver.dump_mode_slices(slices, [](Fid fid) { return fid.raw; });
    ASSERT_EQ(slices.size(), 1u);

    auto resolve = [&](std::uint32_t) -> llvm::StringRef {
        return path;
    };
    CommandResolver same_disk(project);
    same_disk.load_mode_slices(slices, resolve);
    ASSERT_TRUE(same_disk.header_mode(id) == HeaderMode::NeedsContext);

    tmp.touch("h.h", "int y;\n");
    CommandResolver edited(project);
    edited.load_mode_slices(slices, resolve);
    ASSERT_TRUE(edited.header_mode(id) == HeaderMode::Unknown);
}

TEST_CASE(VerdictPersistenceMarksDirty) {
    // The persisted mode slice and the artifacts blob move together: any
    // transition of a content-bound NeedsContext — earned, downgraded by
    // a trial, or reset by a dependency change — must rewrite the blob,
    // or a restart resurrects the dropped verdict (the header's own hash
    // still matches). Session-local transitions must not thrash it.
    FileTable files;
    Project project{files};
    CommandResolver resolver(project);
    auto id = project.file_table.intern(Spelling::absolute("/proj/h.h"));

    resolver.record_header_mode(id, HeaderMode::NeedsContext, 7);
    ASSERT_TRUE(project.artifacts_dirty);

    project.artifacts_dirty = false;
    resolver.reset_header_mode(id);
    ASSERT_TRUE(project.artifacts_dirty);

    // Unbound verdicts and self-contained impressions are never persisted.
    project.artifacts_dirty = false;
    resolver.record_header_mode(id, HeaderMode::NeedsContext);
    resolver.record_header_mode(id, HeaderMode::SelfContained);
    resolver.reset_header_mode(id);
    ASSERT_FALSE(project.artifacts_dirty);

    // A trial downgrading a persisted verdict drops it from the blob.
    resolver.record_header_mode(id, HeaderMode::NeedsContext, 7);
    project.artifacts_dirty = false;
    resolver.record_header_mode(id, HeaderMode::SelfContained);
    ASSERT_TRUE(project.artifacts_dirty);
}

};  // TEST_SUITE(CommandResolver)

}  // namespace
}  // namespace clice::testing
