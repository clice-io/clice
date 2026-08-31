#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "command/argument_parser.h"
#include "sched/context.h"
#include "server/state/session_store.h"

namespace clice::testing {
namespace {

TEST_SUITE(ContextResolver) {

TEST_CASE(ChoiceNeedsSession) {
    TempDir tmp;
    Workspace workspace;
    SessionStore store;
    ContextResolver resolver(workspace);
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
    resolver.saved_contexts[file] =
        SavedContext{no_path_id, std::nullopt, workspace.cdb.entry_hash_hex(pinned)};

    // An open session honors the pinned CDB entry...
    auto session = store.open(file);
    std::string directory;
    std::vector<std::string> arguments;
    resolver.resolve_command(path, directory, arguments, ContextUse::Editor);
    ASSERT_TRUE(llvm::is_contained(arguments, define_of(pinned)));

    // ...but background indexing (no session) must never see user choices.
    arguments.clear();
    resolver.resolve_command(path, directory, arguments, ContextUse::Background);
    ASSERT_TRUE(llvm::is_contained(arguments, define_of(candidates.front().config)));
}

TEST_CASE(PinBaseSurvivesRules) {
    TempDir tmp;
    Workspace workspace;
    SessionStore store;
    ContextResolver resolver(workspace);
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
    resolver.saved_contexts[file] = SavedContext{no_path_id,
                                                 std::nullopt,
                                                 "0123456789abcdef",
                                                 workspace.cdb.entry_hash_hex(pinned)};
    auto session = store.open(file);
    std::string directory;
    std::vector<std::string> arguments;
    resolver.resolve_command(path, directory, arguments, ContextUse::Editor);
    ASSERT_TRUE(llvm::is_contained(arguments, define_of(pinned)));

    // ...while the same stale hash without a base falls back to the default.
    resolver.saved_contexts[file] = SavedContext{no_path_id, std::nullopt, "0123456789abcdef", ""};
    arguments.clear();
    resolver.resolve_command(path, directory, arguments, ContextUse::Editor);
    ASSERT_TRUE(llvm::is_contained(arguments, define_of(candidates.front().config)));
}

TEST_CASE(ValidateKeepsValidChoice) {
    TempDir tmp;
    Workspace workspace;
    SessionStore store;
    ContextResolver resolver(workspace);
    tmp.touch("host.cpp", R"(#include "h.h")");
    tmp.touch("h.h");
    write_cdb(tmp,
              workspace.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("host.cpp"), {}}
    }));

    auto host = workspace.file_table.intern(tmp.path("host.cpp"));
    auto header = workspace.file_table.intern(tmp.path("h.h"));
    workspace.dep_graph.set_includes(host, 0, {header});
    workspace.dep_graph.build_reverse_map();
    resolver.saved_contexts[header] = SavedContext{host, std::nullopt, ""};

    auto session = store.open(header);
    resolver.validate_saved_context(session->path_id);
    ASSERT_TRUE(resolver.saved_contexts.contains(header));
}

TEST_CASE(ValidateDropsStaleChoice) {
    TempDir tmp;
    Workspace workspace;
    SessionStore store;
    ContextResolver resolver(workspace);
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
    resolver.saved_contexts[header] = SavedContext{host, std::nullopt, ""};
    auto header_session = store.open(header);
    resolver.validate_saved_context(header_session->path_id);
    ASSERT_FALSE(resolver.saved_contexts.contains(header));

    // A command pin whose hash matches no current CDB entry.
    resolver.saved_contexts[main_file] = SavedContext{no_path_id, std::nullopt, "deadbeef"};
    auto main_session = store.open(main_file);
    resolver.validate_saved_context(main_session->path_id);
    ASSERT_FALSE(resolver.saved_contexts.contains(main_file));
}

TEST_CASE(InvalidateDropsBorrowed) {
    Workspace workspace;
    ContextResolver resolver(workspace);
    auto borrowed = workspace.file_table.intern("/proj/borrowed.h");
    auto synthesized = workspace.file_table.intern("/proj/synthesized.h");

    // A self-contained borrow tracks no chain deps: forcing re-validation
    // could never trigger anything, so invalidation drops it outright.
    resolver.header_contexts[borrowed] = HeaderContext{};
    resolver.invalidate_header_deps(borrowed);
    ASSERT_FALSE(resolver.header_contexts.contains(borrowed));

    // A synthesized context re-validates its chain by content hash: the
    // shared version's fast path is dropped, the consumed hash stays.
    auto& context = resolver.header_contexts[synthesized];
    context.deps.deps.push_back({.path_id = borrowed, .hash = 7});
    auto vid = workspace.file_table.intern_version(borrowed, 7);
    workspace.file_table.adopt_stamp(vid, 42, 123);
    ASSERT_EQ(workspace.file_table.version(vid).mtime_ns, 123);
    resolver.invalidate_header_deps(synthesized);
    ASSERT_TRUE(resolver.header_contexts.contains(synthesized));
    ASSERT_EQ(workspace.file_table.version(vid).mtime_ns, 0);
    ASSERT_EQ(resolver.header_contexts[synthesized].deps.deps[0].hash, 7u);
}

};  // TEST_SUITE(ContextResolver)

}  // namespace
}  // namespace clice::testing
