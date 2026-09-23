#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "command/argument_parser.h"
#include "server/state/editor_context.h"
#include "support/cache_store.h"

namespace clice::testing {
namespace {

/// A host including a header, with a cache store able to hold the
/// header's synthesized context.
struct HostedHeader {
    TempDir tmp;
    Workspace workspace;
    CommandResolver commands{workspace};
    Fid host;
    Fid header;
    std::string header_path;

    HostedHeader() {
        tmp.touch("host.cpp", "struct S {\n#include \"h.h\"\n};\n");
        tmp.touch("h.h", "int member;\n");
        write_cdb(tmp,
                  workspace.cdb,
                  build_cdb_json({
                      {tmp.root, tmp.path("host.cpp"), {"-DHOSTED"}}
        }));
        host = workspace.file_table.intern(tmp.path("host.cpp"));
        header_path = tmp.path("h.h");
        header = workspace.file_table.intern(header_path);
        workspace.dep_graph.set_includes(host, 0, {{header}});
        workspace.dep_graph.build_reverse_map();

        auto store = CacheStore::open(tmp.path("cache"), 1);
        ASSERT_TRUE(store.has_value());
        store->register_namespace({.name = std::string(header_context_ns),
                                   .extension = ".h",
                                   .policy = CachePolicy::LRU,
                                   .max_bytes = 1ull << 30});
        workspace.store.emplace(std::move(*store));

        auto disk = workspace.file_table.current(header);
        ASSERT_TRUE(disk.has_value());
        commands.record_header_mode(header, HeaderMode::NeedsContext, disk->hash);
    }
};

TEST_SUITE(EditorContext) {

TEST_CASE(SynthesisRecordsEditorHosts) {
    // Only an editor resolution attributes the files it synthesized; a
    // background one leaves the editor's state (and its blob) alone.
    HostedHeader fx;
    EditorContext editor{fx.workspace, fx.commands};
    std::string directory;
    std::vector<std::string> arguments;

    auto background = fx.commands.resolve_command(fx.header_path, directory, arguments);
    ASSERT_EQ(background.source, CommandSource::IncludeGraph);
    ASSERT_EQ(background.host, fx.host);
    ASSERT_FALSE(background.synthesized.empty());
    ASSERT_TRUE(editor.synthesized_hosts.empty());
    ASSERT_TRUE(editor.header_contexts.empty());
    ASSERT_FALSE(editor.dirty);

    auto resolution = editor.resolve_command(fx.header_path, directory, arguments);
    ASSERT_EQ(resolution.source, CommandSource::IncludeGraph);
    ASSERT_FALSE(resolution.synthesized.empty());
    for(auto& file: resolution.synthesized) {
        ASSERT_EQ(editor.synthesized_hosts.lookup(file), fx.host);
    }
    ASSERT_TRUE(editor.dirty);
    auto* context = editor.header_context(fx.header);
    ASSERT_TRUE(context != nullptr);
    ASSERT_FALSE(context->preamble_path.empty());

    // A reuse synthesizes nothing and leaves the blob clean.
    editor.dirty = false;
    auto reused = editor.resolve_command(fx.header_path, directory, arguments);
    ASSERT_TRUE(reused.synthesized.empty());
    ASSERT_FALSE(editor.dirty);
}

TEST_CASE(ArtifactNeedsEditorHost) {
    // An opened artifact compiles under the host the editor recorded for
    // it; background resolution has no such record and never borrows.
    HostedHeader fx;
    EditorContext editor{fx.workspace, fx.commands};
    std::string directory;
    std::vector<std::string> arguments;
    editor.resolve_command(fx.header_path, directory, arguments);
    auto preamble = editor.header_context(fx.header)->preamble_path;
    ASSERT_TRUE(fx.workspace.is_synthesized_artifact(preamble));

    auto opened = editor.resolve_command(preamble, directory, arguments);
    ASSERT_EQ(opened.source, CommandSource::IncludeGraph);
    ASSERT_EQ(opened.host, fx.host);
    ASSERT_TRUE(
        llvm::any_of(arguments, [](llvm::StringRef arg) { return arg.contains("HOSTED"); }));

    auto background = fx.commands.resolve_command(preamble, directory, arguments);
    ASSERT_TRUE(background.source != CommandSource::IncludeGraph);
}

TEST_CASE(GuessedTracksEditorOnly) {
    // The invalidator recompiles every editor-guessed file on a database
    // change; background resolutions must neither add nor clear entries.
    TempDir tmp;
    tmp.touch("lonely.cpp", "");
    tmp.touch("main.cpp", "");
    Workspace workspace;
    CommandResolver commands(workspace);
    EditorContext editor(workspace, commands);
    auto path = tmp.path("lonely.cpp");
    auto file = workspace.file_table.intern(path);
    std::string directory;
    std::vector<std::string> arguments;

    commands.resolve_command(path, directory, arguments);
    ASSERT_FALSE(editor.guessed_commands.contains(file));

    ASSERT_EQ(editor.resolve_command(path, directory, arguments).source, CommandSource::Fallback);
    ASSERT_TRUE(editor.guessed_commands.contains(file));

    write_cdb(tmp,
              workspace.cdb,
              build_cdb_json({
                  {tmp.root, path, {}}
    }));
    commands.resolve_command(path, directory, arguments);
    ASSERT_TRUE(editor.guessed_commands.contains(file));
    ASSERT_EQ(editor.resolve_command(path, directory, arguments).source, CommandSource::CDBExact);
    ASSERT_FALSE(editor.guessed_commands.contains(file));
}

TEST_CASE(PinSteersEditorOnly) {
    TempDir tmp;
    Workspace workspace;
    CommandResolver commands(workspace);
    EditorContext resolver(workspace, commands);
    tmp.touch("main.cpp");
    auto path = tmp.path("main.cpp");
    write_cdb(tmp,
              workspace.cdb,
              build_cdb_json({
                  {tmp.root, path, {"-DFIRST"} },
                  {tmp.root, path, {"-DSECOND"}}
    }));

    auto file = workspace.file_table.intern(path);
    auto candidates = workspace.cdb.candidate_entries(path);
    ASSERT_EQ(candidates.size(), 2u);
    // Pin the non-default candidate (candidate order is content-decided,
    // so the defines are read back rather than assumed).
    auto define_of = [&](ConfigID config) -> llvm::StringRef {
        auto argv = print_argv(workspace.cdb.render_full(config));
        return llvm::StringRef(argv).contains("SECOND") ? "SECOND" : "FIRST";
    };
    auto pinned = candidates.back().config;
    resolver.selections[file] =
        Selection{Fid{}, std::nullopt, workspace.cdb.entry_hash_hex(pinned)};

    // An editor resolution honors the pinned CDB entry...
    std::string directory;
    std::vector<std::string> arguments;
    resolver.resolve_command(path, directory, arguments);
    ASSERT_TRUE(llvm::is_contained(arguments, define_of(pinned)));

    // ...but background indexing must never see user choices.
    arguments.clear();
    commands.resolve_command(path, directory, arguments);
    ASSERT_TRUE(llvm::is_contained(arguments, define_of(candidates.front().config)));
}

TEST_CASE(PinBaseSurvivesRules) {
    TempDir tmp;
    Workspace workspace;
    CommandResolver commands(workspace);
    EditorContext resolver(workspace, commands);
    tmp.touch("main.cpp");
    auto path = tmp.path("main.cpp");
    write_cdb(tmp,
              workspace.cdb,
              build_cdb_json({
                  {tmp.root, path, {"-DFIRST"} },
                  {tmp.root, path, {"-DSECOND"}}
    }));

    auto file = workspace.file_table.intern(path);
    auto candidates = workspace.cdb.candidate_entries(path);
    ASSERT_EQ(candidates.size(), 2u);
    auto define_of = [&](ConfigID config) -> llvm::StringRef {
        auto argv = print_argv(workspace.cdb.render_full(config));
        return llvm::StringRef(argv).contains("SECOND") ? "SECOND" : "FIRST";
    };
    auto pinned = candidates.back().config;

    // A pin whose applied hash went stale (a rule edit since it was saved)
    // but whose base identity is recorded still selects its candidate...
    resolver.selections[file] =
        Selection{Fid{}, std::nullopt, "0123456789abcdef", workspace.cdb.entry_hash_hex(pinned)};
    std::string directory;
    std::vector<std::string> arguments;
    resolver.resolve_command(path, directory, arguments);
    ASSERT_TRUE(llvm::is_contained(arguments, define_of(pinned)));

    // ...while the same stale hash without a base falls back to the default.
    resolver.selections[file] = Selection{Fid{}, std::nullopt, "0123456789abcdef", ""};
    arguments.clear();
    resolver.resolve_command(path, directory, arguments);
    ASSERT_TRUE(llvm::is_contained(arguments, define_of(candidates.front().config)));
}

TEST_CASE(ValidateKeepsValidChoice) {
    TempDir tmp;
    Workspace workspace;
    CommandResolver commands(workspace);
    EditorContext resolver(workspace, commands);
    tmp.touch("host.cpp", R"(#include "h.h")");
    tmp.touch("h.h");
    write_cdb(tmp,
              workspace.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("host.cpp"), {}}
    }));

    auto host = workspace.file_table.intern(tmp.path("host.cpp"));
    auto header = workspace.file_table.intern(tmp.path("h.h"));
    workspace.dep_graph.set_includes(host, 0, {{header}});
    workspace.dep_graph.build_reverse_map();
    resolver.selections[header] = Selection{host, std::nullopt, ""};

    resolver.validate_saved_context(header);
    ASSERT_TRUE(resolver.selections.contains(header));
}

TEST_CASE(ValidateDropsStaleChoice) {
    TempDir tmp;
    Workspace workspace;
    CommandResolver commands(workspace);
    EditorContext resolver(workspace, commands);
    tmp.touch("host.cpp");
    tmp.touch("h.h");
    tmp.touch("main.cpp");
    write_cdb(tmp,
              workspace.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));

    auto host = workspace.file_table.intern(tmp.path("host.cpp"));
    auto header = workspace.file_table.intern(tmp.path("h.h"));
    auto main_file = workspace.file_table.intern(tmp.path("main.cpp"));

    // A host pin whose CDB entry disappeared while the server was down.
    // The drop must dirty the contexts blob, or the stale choice
    // resurrects from disk at the next start.
    resolver.selections[header] = Selection{host, std::nullopt, ""};
    resolver.validate_saved_context(header);
    ASSERT_FALSE(resolver.selections.contains(header));
    ASSERT_TRUE(resolver.dirty);

    // A command pin whose hash matches no current CDB entry.
    resolver.selections[main_file] = Selection{Fid{}, std::nullopt, "deadbeef"};
    resolver.validate_saved_context(main_file);
    ASSERT_FALSE(resolver.selections.contains(main_file));
}

TEST_CASE(InvalidateDropsBorrowed) {
    Workspace workspace;
    CommandResolver commands(workspace);
    EditorContext resolver(workspace, commands);
    auto borrowed = workspace.file_table.intern("/proj/borrowed.h");
    auto synthesized = workspace.file_table.intern("/proj/synthesized.h");

    // A self-contained borrow tracks no chain deps: forcing re-validation
    // could never trigger anything, so invalidation drops it outright.
    resolver.header_contexts[borrowed] = HeaderContext{};
    resolver.invalidate_header_deps(borrowed);
    ASSERT_FALSE(resolver.header_contexts.contains(borrowed));

    // A synthesized context re-validates its chain by content hash: the
    // shared version's fast path is dropped, the consumed version stays.
    auto& context = resolver.header_contexts[synthesized];
    auto vid = workspace.file_table.intern_version(borrowed, 7);
    context.deps.push_back({.path_id = borrowed, .version = vid});
    workspace.file_table.adopt_stamp(vid, 42, 123);
    ASSERT_EQ(workspace.file_table.version(vid).mtime_ns, 123);
    auto stamps = workspace.file_table.stamp_generation;
    resolver.invalidate_header_deps(synthesized);
    ASSERT_TRUE(resolver.header_contexts.contains(synthesized));
    ASSERT_EQ(workspace.file_table.version(vid).mtime_ns, 0);
    ASSERT_EQ(resolver.header_contexts[synthesized].deps[0].version, vid);
    // The revocation is stamp movement — what tells persistence the
    // dropped fast path must not survive in the global blob.
    ASSERT_TRUE(workspace.file_table.stamp_generation != stamps);
}

};  // TEST_SUITE(EditorContext)

}  // namespace
}  // namespace clice::testing
