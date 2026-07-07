#include <memory>
#include <string>

#include "test/test.h"
#include "server/compiler/compiler.h"
#include "server/compiler/context_resolver.h"
#include "server/compiler/indexer.h"
#include "server/service/feature_router.h"
#include "server/state/session_store.h"

namespace clice::testing {
namespace {

TEST_SUITE(FeatureRouter) {

TEST_CASE(DefinitionDegradesOnTimeout) {
    kota::event_loop loop;
    Workspace workspace;
    SessionStore store;
    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);
    IndexQuery index_query(workspace, store);
    Indexer indexer(loop, workspace, pool, contexts, store);
    FeatureRouter router(compiler, index_query, workspace, contexts, indexer);

    auto session = store.open(workspace.path_pool.intern("/proj/a.cpp"));
    store.apply_open(*session, "int x;\n", 1);
    // A compile that never settles: definition must return the (empty)
    // shard answer once the bounded wait expires instead of queueing
    // behind the worker forward without a bound.
    auto pending = std::make_shared<Session::PendingCompile>();
    pending->generation = session->generation;
    session->compiling = pending;

    bool done = false;
    auto body = [&]() -> kota::task<> {
        auto raw = co_await router.definition(session,
                                              "/proj/a.cpp",
                                              protocol::Position{.line = 0, .character = 4});
        CO_ASSERT_TRUE(raw.has_value());
        EXPECT_EQ(raw.value().data, std::string("[]"));
        done = true;
    };
    auto task = body();
    loop.schedule(task);
    loop.run();

    ASSERT_TRUE(done);
    EXPECT_TRUE(session->ast_dirty);
    EXPECT_EQ(session->compiling, pending);
}

};  // TEST_SUITE(FeatureRouter)

}  // namespace

}  // namespace clice::testing
