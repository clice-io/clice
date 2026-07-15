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
    session->quarantine.on_crash();
    session->quarantine.on_crash();

    bool done = false;
    auto body = [&]() -> kota::task<> {
        auto result = co_await compiler.forward_build(worker::BuildKind::Completion, {}, session);
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, worker::dispatch_errc::worker_unavailable);
        // The gate's message, not the empty pool's: without the gate this
        // test would still see worker_unavailable and prove nothing.
        EXPECT_TRUE(result.error().message.contains("quarantined"));
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
        // Two strikes from one request: the retry's death is separate
        // evidence — blame is counted per worker killed, not per request.
        EXPECT_EQ(session.quarantine.crashes(), 2u);

        co_await pool.stop();
        done = true;
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
    EXPECT_TRUE(done);

    logging::reset_anomaly_for_testing();
}

TEST_CASE(QuarantineBlocksFormat) {
    // Formatting is still this document's content on a worker: quarantine
    // refuses it like any other stateless build.
    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);

    auto session = std::make_shared<Session>();
    session->path_id = workspace.path_pool.intern("/proj/poison.cpp");
    session->text = "int x;\n";
    session->quarantine.on_crash();
    session->quarantine.on_crash();

    bool done = false;
    auto body = [&]() -> kota::task<> {
        auto result = co_await compiler.forward_format(session, std::nullopt);
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, worker::dispatch_errc::worker_unavailable);
        EXPECT_TRUE(result.error().message.contains("quarantined"));
        done = true;
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
    EXPECT_TRUE(done);
}

TEST_CASE(GateAnnouncesQuarantine) {
    // A quarantine reached without a compile-failure landing (completion
    // or PCH build tipped the streak) has published nothing; the entry
    // gate must announce it exactly once instead of going silently dead.
    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);

    auto session = std::make_shared<Session>();
    session->path_id = workspace.path_pool.intern("/proj/poison.cpp");
    session->text = "int x;\n";
    session->quarantine.on_crash();
    session->quarantine.on_crash();

    int emits = 0;
    auto conn = compiler.on_output.connect([&](const std::shared_ptr<Session>&) { emits += 1; });

    bool done = false;
    auto body = [&]() -> kota::task<> {
        CO_ASSERT_FALSE(co_await compiler.ensure_compiled(session));
        CO_ASSERT_FALSE(co_await compiler.ensure_compiled(session));
        done = true;
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
    EXPECT_TRUE(done);

    EXPECT_EQ(emits, 1);
    EXPECT_FALSE(session->quarantine.needs_announcement());
    ASSERT_TRUE(session->output.has_value());
    EXPECT_TRUE(session->output->diagnostics.data.contains("quarantined"));
}

TEST_CASE(PchCrashBlocksBuild) {
    // A PCH crash inside a completion build's dependency prep can tip the
    // document into quarantine after the entry gate: the build must stop
    // instead of dispatching the same content to one more worker.
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

    auto session = std::make_shared<Session>();
    session->path_id = workspace.path_pool.intern(src);
    session->text = "#pragma clang __debug crash\n";
    session->quarantine.on_crash();

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        CO_ASSERT_TRUE(pool.start(opts));
        co_await kota::sleep(500);

        auto result = co_await compiler.forward_build(worker::BuildKind::Completion, {}, session);
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, worker::dispatch_errc::worker_unavailable);
        // One inherited strike plus both deaths of the doomed PCH build.
        EXPECT_EQ(session->quarantine.crashes(), 3u);

        co_await pool.stop();
        done = true;
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
    EXPECT_TRUE(done);

    logging::reset_anomaly_for_testing();
}

TEST_CASE(PoisonPreambleBudget) {
    // One document's quarantine cannot contain a poison preamble: the PCH
    // is shared, so every session with the same preamble would re-trigger
    // the build and burn workers of its own. After `threshold` crashed
    // builds the key itself is refused — before any dispatch, which is why
    // the third session records no crash at all.
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

    auto make_session = [&] {
        Session session;
        session.path_id = workspace.path_pool.intern(src);
        session.text = "#pragma clang __debug crash\n";
        return session;
    };
    auto first = make_session();
    auto second = make_session();
    auto third = make_session();

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

        auto build = [&](Session& session) {
            return CompilerFixture::ensure_pch(compiler,
                                               session,
                                               session.generation,
                                               session.dirty_epoch,
                                               directory,
                                               arguments);
        };
        // One request, two dead workers, two strikes: the key blocks after
        // a single poison build instead of burning workers for a second
        // session's attempt.
        CO_ASSERT_FALSE(co_await build(first));
        EXPECT_EQ(first.quarantine.crashes(), 2u);

        // Refused without touching a worker.
        CO_ASSERT_FALSE(co_await build(second));
        EXPECT_EQ(second.quarantine.crashes(), 0u);
        CO_ASSERT_FALSE(co_await build(third));
        EXPECT_EQ(third.quarantine.crashes(), 0u);

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
