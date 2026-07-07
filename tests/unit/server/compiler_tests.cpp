#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "test/test.h"
#include "server/compiler/compiler.h"
#include "server/compiler/context_resolver.h"

namespace clice::testing {

/// Reaches Compiler's private compile-preparation steps for guard tests.
struct CompilerFixture {
    static kota::task<bool> ensure_pch(Compiler& compiler,
                                       Session& session,
                                       std::uint64_t launch_generation,
                                       std::uint64_t launch_epoch,
                                       const std::string& directory,
                                       const std::vector<std::string>& arguments) {
        return compiler.ensure_pch(session, launch_generation, launch_epoch, directory, arguments);
    }
};

namespace {

TEST_SUITE(CompilerGuards) {

TEST_CASE(EpochGuardsPchWrite) {
    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);

    Session session;
    session.path_id = workspace.path_pool.intern("/proj/a.cpp");
    // No preamble directives: a current round would take the pch_ref reset
    // branch; an invalidated continuation must not touch it.
    session.text = "int x;";
    session.pch_ref = Session::PCHRef{"key", 0};

    auto gen = session.generation;
    auto epoch = session.dirty_epoch;
    // A Lost-type invalidation (disk/CDB change behind the in-flight
    // round) lands after takeoff: dirty_epoch bumps, generation stays.
    session.dirty_epoch += 1;

    std::string directory = "/proj";
    std::vector<std::string> arguments = {"clang++", "-fsyntax-only", "/proj/a.cpp"};
    bool wrote = true;
    auto body = [&]() -> kota::task<> {
        wrote = co_await CompilerFixture::ensure_pch(compiler,
                                                     session,
                                                     gen,
                                                     epoch,
                                                     directory,
                                                     arguments);
    };
    auto task = body();
    loop.schedule(task);
    loop.run();

    EXPECT_FALSE(wrote);
    // The stale continuation left the session's PCH reference untouched.
    ASSERT_TRUE(session.pch_ref.has_value());
    EXPECT_EQ(session.pch_ref->key, std::string("key"));
}

TEST_CASE(BoundedWaitTimesOut) {
    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);

    auto session = std::make_shared<Session>();
    session->path_id = workspace.path_pool.intern("/proj/a.cpp");
    session->ast_dirty = true;
    // An in-flight compile that never settles: the bounded wait must give
    // up instead of queueing behind it indefinitely.
    auto pending = std::make_shared<Session::PendingCompile>();
    pending->generation = session->generation;
    session->compiling = pending;

    bool fresh = true;
    auto body = [&]() -> kota::task<> {
        fresh = co_await compiler.ensure_compiled_bounded(session, std::chrono::milliseconds(50));
    };
    auto task = body();
    loop.schedule(task);
    loop.run();

    EXPECT_FALSE(fresh);
    EXPECT_TRUE(session->ast_dirty);
    // Only the wait was abandoned; the in-flight compile is untouched.
    EXPECT_EQ(session->compiling, pending);
}

TEST_CASE(BoundedWaitFastPath) {
    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);

    auto session = std::make_shared<Session>();
    session->path_id = workspace.path_pool.intern("/proj/a.cpp");
    session->ast_dirty = false;

    bool fresh = false;
    auto body = [&]() -> kota::task<> {
        fresh = co_await compiler.ensure_compiled_bounded(session, std::chrono::milliseconds(50));
    };
    auto task = body();
    loop.schedule(task);
    loop.run();

    EXPECT_TRUE(fresh);
    // A clean session neither waits nor launches a compile.
    EXPECT_EQ(session->compiling, nullptr);
}

};  // TEST_SUITE(CompilerGuards)

}  // namespace

}  // namespace clice::testing
