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

};  // TEST_SUITE(ContextResolver)

}  // namespace
}  // namespace clice::testing
