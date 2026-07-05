#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "command/argument_parser.h"
#include "server/context/context_resolver.h"
#include "server/session/session_store.h"

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

    auto file = workspace.path_pool.intern(path);
    auto results = workspace.cdb.lookup(path, {});
    ASSERT_EQ(results.size(), 2u);
    resolver.saved_contexts[file] = SavedContext{
        no_path_id,
        std::nullopt,
        canonical_command_hash(results[1].to_string_argv(), results[1].resolved.directory)};

    // An open session honors the pinned CDB entry...
    auto session = store.open(file);
    std::string directory;
    std::vector<std::string> arguments;
    resolver.resolve_command(path, directory, arguments, session.get());
    ASSERT_TRUE(llvm::is_contained(arguments, "SECOND"));

    // ...but background indexing (no session) must never see user choices.
    arguments.clear();
    resolver.resolve_command(path, directory, arguments, nullptr);
    ASSERT_TRUE(llvm::is_contained(arguments, "FIRST"));
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

    auto host = workspace.path_pool.intern(tmp.path("host.cpp"));
    auto header = workspace.path_pool.intern(tmp.path("h.h"));
    workspace.dep_graph.set_includes(host, 0, {header});
    workspace.dep_graph.build_reverse_map();
    resolver.saved_contexts[header] = SavedContext{host, std::nullopt, ""};

    auto session = store.open(header);
    resolver.validate_saved_context(*session);
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

    auto host = workspace.path_pool.intern(tmp.path("host.cpp"));
    auto header = workspace.path_pool.intern(tmp.path("h.h"));
    auto main_file = workspace.path_pool.intern(tmp.path("main.cpp"));

    // A host pin whose CDB entry disappeared while the server was down.
    resolver.saved_contexts[header] = SavedContext{host, std::nullopt, ""};
    auto header_session = store.open(header);
    resolver.validate_saved_context(*header_session);
    ASSERT_FALSE(resolver.saved_contexts.contains(header));

    // A command pin whose hash matches no current CDB entry.
    resolver.saved_contexts[main_file] = SavedContext{no_path_id, std::nullopt, "deadbeef"};
    auto main_session = store.open(main_file);
    resolver.validate_saved_context(*main_session);
    ASSERT_FALSE(resolver.saved_contexts.contains(main_file));
}

TEST_CASE(InvalidateDropsBorrowed) {
    Workspace workspace;
    ContextResolver resolver(workspace);
    auto borrowed = workspace.path_pool.intern("/proj/borrowed.h");
    auto synthesized = workspace.path_pool.intern("/proj/synthesized.h");

    // A self-contained borrow tracks no chain deps: zeroing its baseline
    // could never force re-validation, so invalidation drops it outright.
    resolver.header_contexts[borrowed] = HeaderContext{};
    resolver.invalidate_header_deps(borrowed);
    ASSERT_FALSE(resolver.header_contexts.contains(borrowed));

    // A synthesized context re-validates its chain by content hash.
    auto& context = resolver.header_contexts[synthesized];
    context.deps.path_ids = {borrowed};
    context.deps.hashes = {7};
    context.deps.build_at = 123;
    resolver.invalidate_header_deps(synthesized);
    ASSERT_TRUE(resolver.header_contexts.contains(synthesized));
    ASSERT_EQ(resolver.header_contexts[synthesized].deps.build_at, 0);
}

};  // TEST_SUITE(ContextResolver)

}  // namespace
}  // namespace clice::testing
