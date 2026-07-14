#include <string>
#include <vector>

#include "test/temp_dir.h"
#include "test/test.h"
#include "server/compiler/compiler.h"
#include "server/compiler/context_resolver.h"
#include "server/worker_test_helpers.h"
#include "support/anomaly.h"
#include "support/cache_store.h"

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
    // No preamble directives: a current round would take the pch_key reset
    // branch; an invalidated continuation must not touch it.
    session.text = "int x;";
    session.pch_key = "key";

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
    ASSERT_TRUE(session.pch_key.has_value());
    EXPECT_EQ(*session.pch_key, std::string("key"));
}

TEST_CASE(QuarantineBlocksBuilds) {
    // A quarantined document gets no stateless builds either: completion
    // requests compile the same content the quarantine watches.
    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);

    auto session = std::make_shared<Session>();
    session->path_id = workspace.path_pool.intern("/proj/poison.cpp");
    session->text = "int x;\n";
    session->compile_crash_streak = Session::quarantine_threshold;

    bool done = false;
    auto body = [&]() -> kota::task<> {
        auto result = co_await compiler.forward_build(worker::BuildKind::Completion, {}, session);
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, worker::dispatch_errc::worker_unavailable);
        done = true;
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
    EXPECT_TRUE(done);
}

TEST_CASE(PchCrashCountsStreak) {
    // A PCH build that kills its stateless worker must count toward the
    // document's quarantine streak: the preamble is the document's content
    // too, and without this a poison preamble never quarantines.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    kota::event_loop loop;
    Workspace workspace;
    auto store = CacheStore::open(tmp.path("root"), 1);
    ASSERT_TRUE(store.has_value());
    store->register_namespace({.name = "pch",
                               .extension = ".pch",
                               .aux_extension = ".pch.idx",
                               .policy = CachePolicy::LRU,
                               .max_bytes = 1ull << 30});
    workspace.store.emplace(std::move(*store));

    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);

    Session session;
    session.path_id = workspace.path_pool.intern(src);
    session.text = "#pragma clang __debug crash\n";

    std::string directory = tmp.path(".");
    auto arguments = make_args(src);

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        CO_ASSERT_TRUE(pool.start(opts));
        co_await kota::sleep(500);

        bool built = co_await CompilerFixture::ensure_pch(compiler,
                                                          session,
                                                          session.generation,
                                                          session.dirty_epoch,
                                                          directory,
                                                          arguments);
        EXPECT_FALSE(built);
        EXPECT_EQ(session.compile_crash_streak, 1u);

        co_await pool.stop();
        done = true;
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
    EXPECT_TRUE(done);

    logging::reset_anomaly_for_testing();
}

};  // TEST_SUITE(CompilerGuards)

}  // namespace

}  // namespace clice::testing
