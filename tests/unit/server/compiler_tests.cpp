#include <format>
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

TEST_CASE(StopUnblocksCompileWaiters) {
    // Compiler::stop() cancels compile_tasks, destroying a suspended
    // run_compile frame without resuming it. The finish must be RAII: a
    // waiter parked on the pending compile's `done` event would otherwise
    // hang forever and the session would stay bricked behind `compiling`.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("slow.cpp", "");
    auto src = tmp.path("slow.cpp");

    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);

    auto session = std::make_shared<Session>();
    session->path_id = workspace.path_pool.intern(src);
    session->text =
        "#include <vector>\n#include <string>\n#include <algorithm>\n" "#include <regex>\nint main() { return 0; }\n";

    bool waiter_done = false;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        CO_ASSERT_TRUE(pool.start(opts));
        co_await kota::sleep(500);

        kota::task_group<> group(loop);
        auto waiter = [&]() -> kota::task<> {
            [[maybe_unused]] bool ok = co_await compiler.ensure_compiled(session);
            waiter_done = true;
        };
        group.spawn(waiter());

        // Deterministic in-flight state: the pending compile is registered
        // and the waiter still parked, so stop() provably cancels a
        // suspended run_compile instead of racing one that already landed.
        for(int i = 0; i < 100 && session->compiling == nullptr; ++i) {
            co_await kota::sleep(10);
        }
        CO_ASSERT_TRUE(session->compiling != nullptr);
        CO_ASSERT_FALSE(waiter_done);
        co_await compiler.stop();

        // Bounded: a waiter left hanging (the pre-RAII bug) fails this
        // cleanly here instead of hanging the whole binary.
        for(int i = 0; i < 100 && !waiter_done; ++i) {
            co_await kota::sleep(100);
        }
        if(!waiter_done) {
            group.cancel();
        }
        co_await group.join();

        EXPECT_TRUE(waiter_done);
        EXPECT_TRUE(session->compiling == nullptr);

        co_await pool.stop();
        done = true;
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
    EXPECT_TRUE(done);

    logging::reset_anomaly_for_testing();
}

TEST_CASE(AbandonCancelsDepsScope) {
    // The two supersede entry points differ on deps_scope by design:
    // abandon_superseded (the edit path, no replacement round) cancels the
    // stale round's dependency waits; interrupt_superseded (the supersede
    // point) must not — its deps cancel is ordered after the replacement
    // spawn so module interest never dips to zero across the swap.
    TempDir tmp;
    tmp.touch("scoped.cpp", "");

    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);

    auto session = std::make_shared<Session>();
    session->path_id = workspace.path_pool.intern(tmp.path("scoped.cpp"));
    session->compiling = std::make_shared<Session::PendingCompile>();
    session->compiling->generation = session->generation;

    // Current round: both are no-ops.
    compiler.abandon_superseded(*session);
    EXPECT_FALSE(session->compiling->deps_scope.cancelled());

    session->generation += 1;

    compiler.interrupt_superseded(*session);
    EXPECT_FALSE(session->compiling->deps_scope.cancelled());

    compiler.abandon_superseded(*session);
    EXPECT_TRUE(session->compiling->deps_scope.cancelled());
}

TEST_CASE(EditInterruptsStaleCompile) {
    // The didChange path: an edit with NO follow-up request interrupts the
    // in-flight parse via interrupt_superseded. The superseded round's
    // waiter resolves false (its result is for a buffer that no longer
    // exists — the editor re-requests after an edit) instead of sitting
    // behind a stale 200k-declaration parse, and the next request compiles
    // the fresh content. Liveness pin; the interruption content is pinned
    // by StatefulWorker.CancelNotificationInterruptsCompile.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("edited_only.cpp", "");
    auto src = tmp.path("edited_only.cpp");

    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);

    auto session = std::make_shared<Session>();
    session->path_id = workspace.path_pool.intern(src);
    std::string text;
    text.reserve(1 << 22);
    for(int i = 0; i < 200'000; ++i) {
        text += std::format("int v{};\n", i);
    }
    session->text = std::move(text);

    bool waiter_done = false;
    bool waiter_ok = false;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        CO_ASSERT_TRUE(pool.start(opts));
        co_await kota::sleep(500);

        kota::task_group<> group(loop);
        auto waiter = [&]() -> kota::task<> {
            waiter_ok = co_await compiler.ensure_compiled(session);
            waiter_done = true;
        };
        group.spawn(waiter());

        for(int i = 0; i < 100 && session->compiling == nullptr; ++i) {
            co_await kota::sleep(10);
        }
        CO_ASSERT_TRUE(session->compiling != nullptr);

        // What the didChange handler does: fold the edit in, then interrupt.
        session->text = "int fixed;\n";
        session->generation += 1;
        session->ast_dirty = true;
        compiler.interrupt_superseded(*session);

        for(int i = 0; i < 600 && !waiter_done; ++i) {
            co_await kota::sleep(100);
        }
        if(!waiter_done) {
            group.cancel();
        }
        co_await group.join();

        CO_ASSERT_TRUE(waiter_done);
        EXPECT_FALSE(waiter_ok);
        EXPECT_TRUE(session->compiling == nullptr);

        // The next request (the editor re-queries after an edit) compiles
        // the fresh content.
        bool second_ok = co_await compiler.ensure_compiled(session);
        EXPECT_TRUE(second_ok);
        EXPECT_FALSE(session->ast_dirty);

        co_await compiler.stop();
        co_await pool.stop();
        done = true;
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
    EXPECT_TRUE(done);

    logging::reset_anomaly_for_testing();
}

TEST_CASE(SupersededCompileCancelled) {
    // An edit mid-compile supersedes the in-flight round: the waiter breaks
    // out, the supersede point interrupts the worker's parse with a
    // CancelCompile notification, and the replacement compiles the new
    // content. This pins the supersede path's liveness (both waiters
    // resolve, the fresh AST lands); the interruption itself is pinned
    // content-wise by StatefulWorker.CancelNotificationInterruptsCompile.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("edited.cpp", "");
    auto src = tmp.path("edited.cpp");

    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);

    auto session = std::make_shared<Session>();
    session->path_id = workspace.path_pool.intern(src);
    std::string text;
    text.reserve(1 << 22);
    for(int i = 0; i < 200'000; ++i) {
        text += std::format("int v{};\n", i);
    }
    session->text = std::move(text);

    bool first_done = false;
    bool second_ok = false;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        CO_ASSERT_TRUE(pool.start(opts));
        co_await kota::sleep(500);

        kota::task_group<> group(loop);
        auto first = [&]() -> kota::task<> {
            [[maybe_unused]] bool ok = co_await compiler.ensure_compiled(session);
            first_done = true;
        };
        group.spawn(first());

        for(int i = 0; i < 100 && session->compiling == nullptr; ++i) {
            co_await kota::sleep(10);
        }
        CO_ASSERT_TRUE(session->compiling != nullptr);

        // The edit lands while the slow compile is in flight.
        session->text = "int fixed;\n";
        session->generation += 1;

        auto second = [&]() -> kota::task<> {
            second_ok = co_await compiler.ensure_compiled(session);
        };
        group.spawn(second());

        for(int i = 0; i < 600 && !(first_done && second_ok); ++i) {
            co_await kota::sleep(100);
        }
        if(!(first_done && second_ok)) {
            group.cancel();
        }
        co_await group.join();

        EXPECT_TRUE(first_done);
        EXPECT_TRUE(second_ok);
        EXPECT_TRUE(session->compiling == nullptr);
        EXPECT_FALSE(session->ast_dirty);

        co_await compiler.stop();
        co_await pool.stop();
        done = true;
    };
    auto task = body();
    loop.schedule(task);
    loop.run();
    EXPECT_TRUE(done);

    logging::reset_anomaly_for_testing();
}

TEST_CASE(ClientCancelSparesCompile) {
    // A client's $/cancelRequest tears down one request's frame, never the
    // shared compile it waits on: the detached round serves every waiter.
    // A regression that threads the request token into the shared round
    // would kill waiter B's result along with waiter A's frame.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("shared.cpp", "");
    auto src = tmp.path("shared.cpp");

    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts(workspace);
    WorkerPool pool(loop);
    Compiler compiler(loop, workspace, contexts, pool);

    auto session = std::make_shared<Session>();
    session->path_id = workspace.path_pool.intern(src);
    std::string text;
    text.reserve(1 << 21);
    for(int i = 0; i < 50'000; ++i) {
        text += std::format("int v{};\n", i);
    }
    session->text = std::move(text);
    session->line_starts = kota::ipc::lsp::build_line_starts(session->text);

    bool cancelled_returned = false;
    bool other_answered = false;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        CO_ASSERT_TRUE(pool.start(opts));
        co_await kota::sleep(500);

        kota::cancellation_source source;
        kota::task_group<> group(loop);
        auto cancelled_waiter = [&]() -> kota::task<> {
            auto hover = [&]() -> Compiler::RawResult {
                co_return co_await compiler.forward_query(worker::QueryKind::Hover,
                                                          session,
                                                          protocol::Position{0, 4},
                                                          {},
                                                          source.token());
            };
            auto r = co_await kota::with_token(hover(), source.token());
            cancelled_returned = r.is_cancelled();
        };
        auto other_waiter = [&]() -> kota::task<> {
            auto result = co_await compiler.forward_query(worker::QueryKind::Hover,
                                                          session,
                                                          protocol::Position{0, 4});
            other_answered = result.has_value();
        };
        group.spawn(cancelled_waiter());
        group.spawn(other_waiter());

        for(int i = 0; i < 100 && session->compiling == nullptr; ++i) {
            co_await kota::sleep(10);
        }
        CO_ASSERT_TRUE(session->compiling != nullptr);
        source.cancel();

        for(int i = 0; i < 600 && !other_answered; ++i) {
            co_await kota::sleep(100);
        }
        if(!other_answered) {
            group.cancel();
        }
        co_await group.join();

        EXPECT_TRUE(cancelled_returned);
        EXPECT_TRUE(other_answered);
        EXPECT_FALSE(session->ast_dirty);
        EXPECT_TRUE(session->compiling == nullptr);

        co_await compiler.stop();
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
