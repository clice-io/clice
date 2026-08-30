#include <format>
#include <string>
#include <vector>

#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "sched/context.h"
#include "sched/families/pch.h"
#include "sched/families/pcm.h"
#include "sched/graph.h"
#include "server/service/ast_family.h"
#include "server/service/worker_forwarder.h"
#include "server/worker_test_helpers.h"
#include "support/anomaly.h"
#include "support/cache_store.h"
#include "syntax/dependency_graph.h"

namespace clice::testing {

/// Reaches the forwarder's private input-preparation steps for guard tests.
struct ForwarderFixture {
    static kota::task<bool> ensure_pch(WorkerForwarder& forwarder,
                                       const std::shared_ptr<Session>& session,
                                       std::uint64_t license_generation,
                                       std::uint64_t license_epoch,
                                       const std::string& directory,
                                       const std::vector<std::string>& arguments) {
        return forwarder.ensure_pch(session,
                                    license_generation,
                                    license_epoch,
                                    directory,
                                    arguments);
    }
};

namespace {

/// The full server-side compile stack, workerless until a test starts the
/// pool. Sessions are opened through the store so the family's runner
/// resolves them.
struct Stack {
    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts{workspace};
    WorkerPool pool{loop};
    TaskGraph graph{loop};
    PCMFamily pcm{graph, workspace, contexts, pool};
    PCHFamily pch{graph, workspace, contexts, pool};
    SessionStore sessions;
    ASTFamily ast{workspace, contexts, graph, pcm, pch, pool, sessions, loop};
    WorkerForwarder forwarder{workspace, contexts, pcm, pch, ast, pool};

    Stack() {
        pcm.register_runner();
        pch.register_runner();
        ast.register_runner();
    }

    std::shared_ptr<Session> open(llvm::StringRef path, std::string text) {
        auto session = sessions.open(workspace.path_pool.intern(path));
        session->text = std::move(text);
        session->line_starts = kota::ipc::lsp::build_line_starts(session->text);
        return session;
    }

    bool is_compiling(std::uint32_t path_id) const {
        return graph.is_compiling({ast_family, path_id});
    }

    void register_pch_store(TempDir& tmp) {
        auto store = CacheStore::open(tmp.path("root"), 1);
        ASSERT_TRUE(store.has_value());
        store->register_namespace({.name = "pch",
                                   .extension = ".pch",
                                   .aux_extension = ".pch.idx",
                                   .policy = CachePolicy::LRU,
                                   .max_bytes = 1ull << 30});
        workspace.store.emplace(std::move(*store));
    }
};

TEST_SUITE(ASTFamilyGuards) {

TEST_CASE(SupersedeTouchesEntry) {
    Stack stack;
    auto session = stack.open("/proj/a.cpp", "int x;\n");
    auto pid = session->path_id;
    stack.ast.projections.entries[pid].current = true;
    auto epoch = stack.ast.projections.epoch(pid);

    stack.ast.supersede(pid);

    ASSERT_FALSE(stack.ast.projections.current(pid));
    ASSERT_EQ(stack.ast.projections.epoch(pid), epoch + 1);
}

TEST_CASE(InvalidateKeepsProjection) {
    Stack stack;
    auto session = stack.open("/proj/a.cpp", "int x;\n");
    auto pid = session->path_id;
    stack.ast.projections.set_pch_key(pid, "key");
    stack.ast.projections.entries[pid].current = true;

    stack.ast.invalidate(pid);

    // Only the currency claim is revoked; the products stay for the
    // bounded-staleness consumers (preamble links, hover gap).
    ASSERT_FALSE(stack.ast.projections.current(pid));
    auto projection = stack.ast.projections.projection(pid);
    ASSERT_TRUE(projection != nullptr);
    ASSERT_TRUE(projection->pch_key.has_value());
}

TEST_CASE(DropErasesEntry) {
    Stack stack;
    auto session = stack.open("/proj/a.cpp", "int x;\n");
    auto pid = session->path_id;
    stack.ast.projections.set_pch_key(pid, "key");

    stack.ast.drop(pid);

    ASSERT_TRUE(stack.ast.projections.projection(pid) == nullptr);
    ASSERT_FALSE(stack.ast.projections.current(pid));
}

TEST_CASE(SwitchIdentityResets) {
    Stack stack;
    auto session = stack.open("/proj/h.h", "int x;\n");
    auto pid = session->path_id;
    session->trial_done = true;
    auto& entry = stack.ast.projections.entries[pid];
    auto projection = std::make_shared<ASTProjection>();
    projection->pch_key = "key";
    projection->output = CompileOutput{.version = 3, .source = CommandSource::CDBExact};
    entry.projection = std::move(projection);
    entry.deps.emplace();
    entry.current = true;
    auto generation = session->generation;

    stack.ast.switch_identity(*session);

    // The new context is a different compilation identity: the state
    // earned under the old one is dropped, but the published output stays
    // until the next compile overwrites it (same as the old world's
    // session fields).
    ASSERT_EQ(session->generation, generation + 1);
    ASSERT_FALSE(session->trial_done);
    ASSERT_FALSE(stack.ast.projections.current(pid));
    ASSERT_FALSE(stack.ast.projections.entries[pid].deps.has_value());
    auto after = stack.ast.projections.projection(pid);
    ASSERT_FALSE(after->pch_key.has_value());
    ASSERT_TRUE(after->output.has_value());
}

TEST_CASE(GateAnnouncesQuarantine) {
    // A quarantine reached without a compile-failure landing (completion
    // or PCH build tipped the streak) has published nothing; the entry
    // gate must announce it exactly once instead of going silently dead.
    Stack stack;
    auto session = stack.open("/proj/poison.cpp", "int x;\n");
    session->quarantine.on_crash();
    session->quarantine.on_crash();

    int emits = 0;
    auto conn = stack.ast.on_output.connect([&](const std::shared_ptr<Session>&) { emits += 1; });

    bool done = false;
    auto body = [&]() -> kota::task<> {
        CO_ASSERT_FALSE(co_await stack.ast.ensure_compiled(session));
        CO_ASSERT_FALSE(co_await stack.ast.ensure_compiled(session));
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);

    EXPECT_EQ(emits, 1);
    EXPECT_FALSE(session->quarantine.needs_announcement());
    auto projection = stack.ast.projections.projection(session->path_id);
    ASSERT_TRUE(projection && projection->output.has_value());
    EXPECT_TRUE(projection->output->diagnostics.data.contains("quarantined"));
}

TEST_CASE(ShutdownUnblocksWaiters) {
    // A waiter parked on an in-flight round must resolve at shutdown:
    // ASTFamily::stop interrupts the parse (the send itself carries no
    // token by design), so the round lands promptly and the graph's
    // shutdown finds nothing to wait out.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("slow.cpp", "");
    auto src = tmp.path("slow.cpp");

    Stack stack;
    std::string text;
    text.reserve(1 << 22);
    for(int i = 0; i < 200'000; ++i) {
        text += std::format("int v{};\n", i);
    }
    auto session = stack.open(src, std::move(text));

    bool waiter_done = false;
    bool waiter_ok = true;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        CO_ASSERT_TRUE(stack.pool.start(opts));

        kota::task_group<> group(stack.loop);
        auto waiter = [&]() -> kota::task<> {
            waiter_ok = co_await stack.ast.ensure_compiled(session);
            waiter_done = true;
        };
        group.spawn(waiter());

        for(int i = 0; i < 100 && !stack.is_compiling(session->path_id); ++i) {
            co_await kota::sleep(10);
        }
        CO_ASSERT_TRUE(stack.is_compiling(session->path_id));
        CO_ASSERT_FALSE(waiter_done);

        co_await stack.ast.stop();

        // Bounded: a waiter left hanging fails this cleanly here instead
        // of hanging the whole binary.
        for(int i = 0; i < 100 && !waiter_done; ++i) {
            co_await kota::sleep(100);
        }
        if(!waiter_done) {
            group.cancel();
        }
        co_await group.join();

        EXPECT_TRUE(waiter_done);
        EXPECT_FALSE(waiter_ok);
        EXPECT_FALSE(stack.is_compiling(session->path_id));

        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);

    logging::reset_anomaly_for_testing();
}

TEST_CASE(EditInterruptsStaleCompile) {
    // The didChange path: an edit with NO follow-up request supersedes the
    // in-flight round — the CancelCompile interrupt makes the worker
    // abandon the stale parse, the waiter resolves false (its result is
    // for a buffer that no longer exists — the editor re-requests after
    // an edit), and the next request compiles the fresh content. Liveness
    // pin; the interruption content is pinned by
    // StatefulWorker.CancelNotificationInterruptsCompile.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("edited_only.cpp", "");
    auto src = tmp.path("edited_only.cpp");

    Stack stack;
    std::string text;
    text.reserve(1 << 22);
    for(int i = 0; i < 200'000; ++i) {
        text += std::format("int v{};\n", i);
    }
    auto session = stack.open(src, std::move(text));

    bool waiter_done = false;
    bool waiter_ok = false;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        CO_ASSERT_TRUE(stack.pool.start(opts));

        kota::task_group<> group(stack.loop);
        auto waiter = [&]() -> kota::task<> {
            waiter_ok = co_await stack.ast.ensure_compiled(session);
            waiter_done = true;
        };
        group.spawn(waiter());

        for(int i = 0; i < 100 && !stack.is_compiling(session->path_id); ++i) {
            co_await kota::sleep(10);
        }
        CO_ASSERT_TRUE(stack.is_compiling(session->path_id));

        // What the didChange handler does: fold the edit in, then supersede.
        session->text = "int fixed;\n";
        session->line_starts = kota::ipc::lsp::build_line_starts(session->text);
        session->generation += 1;
        stack.ast.supersede(session->path_id);

        for(int i = 0; i < 600 && !waiter_done; ++i) {
            co_await kota::sleep(100);
        }
        if(!waiter_done) {
            group.cancel();
        }
        co_await group.join();

        CO_ASSERT_TRUE(waiter_done);
        EXPECT_FALSE(waiter_ok);

        // The next request (the editor re-queries after an edit) compiles
        // the fresh content.
        bool second_ok = co_await stack.ast.ensure_compiled(session);
        EXPECT_TRUE(second_ok);
        EXPECT_TRUE(stack.ast.projections.current(session->path_id));

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);

    logging::reset_anomaly_for_testing();
}

TEST_CASE(SupersededCompileCancelled) {
    // An edit mid-compile supersedes the in-flight round: the first waiter
    // abandons (its buffer is gone), the supersede point interrupts the
    // worker's parse, and the second waiter's respawned round compiles the
    // new content. This pins the supersede path's liveness; the
    // interruption itself is pinned content-wise by
    // StatefulWorker.CancelNotificationInterruptsCompile.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("edited.cpp", "");
    auto src = tmp.path("edited.cpp");

    Stack stack;
    std::string text;
    text.reserve(1 << 22);
    for(int i = 0; i < 200'000; ++i) {
        text += std::format("int v{};\n", i);
    }
    auto session = stack.open(src, std::move(text));

    bool first_done = false;
    bool second_ok = false;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        CO_ASSERT_TRUE(stack.pool.start(opts));

        kota::task_group<> group(stack.loop);
        auto first = [&]() -> kota::task<> {
            [[maybe_unused]] bool ok = co_await stack.ast.ensure_compiled(session);
            first_done = true;
        };
        group.spawn(first());

        for(int i = 0; i < 100 && !stack.is_compiling(session->path_id); ++i) {
            co_await kota::sleep(10);
        }
        CO_ASSERT_TRUE(stack.is_compiling(session->path_id));

        // The edit lands while the slow compile is in flight.
        session->text = "int fixed;\n";
        session->line_starts = kota::ipc::lsp::build_line_starts(session->text);
        session->generation += 1;
        stack.ast.supersede(session->path_id);

        auto second = [&]() -> kota::task<> {
            second_ok = co_await stack.ast.ensure_compiled(session);
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
        EXPECT_TRUE(stack.ast.projections.current(session->path_id));

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);

    logging::reset_anomaly_for_testing();
}

TEST_CASE(BufferImportBuildsPCM) {
    // Imports resolve from the buffer, not the disk: the on-disk TU has
    // no import, so only the round's buffer scan can discover `import m;`
    // and build the PCM before the compile consumes it.
    TempDir tmp;
    tmp.touch("m.cppm",
              "export module m;\n"
              "export int mv() { return 1; }\n");
    tmp.touch("main.cpp", "int main() { return 0; }\n");
    auto src = tmp.path("main.cpp");

    Stack stack;
    write_cdb(tmp,
              stack.workspace.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"), {}},
                  {tmp.root, src,                {}},
    }));
    scan_dependency_graph(stack.workspace.cdb, stack.workspace.dep_graph);
    stack.workspace.dep_graph.build_reverse_map();
    stack.workspace.build_module_map();

    auto store = CacheStore::open(tmp.path("root"), 1);
    ASSERT_TRUE(store.has_value());
    store->register_namespace({.name = "pch",
                               .extension = ".pch",
                               .aux_extension = ".pch.idx",
                               .policy = CachePolicy::LRU,
                               .max_bytes = 1ull << 30});
    store->register_namespace(
        {.name = "pcm", .extension = ".pcm", .policy = CachePolicy::LRU, .max_bytes = 1ull << 30});
    stack.workspace.store.emplace(std::move(*store));

    auto session = stack.open(src, "import m;\nint main() { return mv(); }\n");

    bool ok = false;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 1;
        CO_ASSERT_TRUE(stack.pool.start(opts));

        ok = co_await stack.ast.ensure_compiled(session);

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(stack.ast.projections.current(session->path_id));
    auto mod_ids = stack.workspace.dep_graph.lookup_module("m");
    ASSERT_FALSE(mod_ids.empty());
    EXPECT_TRUE(stack.workspace.pcm_cache.contains(mod_ids[0]));
}

TEST_CASE(BufferImportRecorded) {
    // Zero-provider window: the compile fails on the unresolved import,
    // but the buffer scan must still record the name — the first
    // provider's arrival has nothing else to find this document by (the
    // disk candidate set cannot see unsaved edits).
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("main.cpp", "int main() { return 0; }\n");
    auto src = tmp.path("main.cpp");

    Stack stack;
    auto session = stack.open(src, "import m;\nint main() { return 0; }\n");

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        CO_ASSERT_TRUE(stack.pool.start(opts));

        [[maybe_unused]] bool ok = co_await stack.ast.ensure_compiled(session);

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);

    // The sentinel edge survives the landing: a first provider's update
    // must reach this document's node.
    auto dirtied = stack.graph.update(PCMFamily::unresolved_node("m"));
    EXPECT_TRUE(std::ranges::find(dirtied, NodeId{ast_family, session->path_id}) != dirtied.end());

    logging::reset_anomaly_for_testing();
}

TEST_CASE(IncludeImportRecorded) {
    // Zero-provider window, import introduced by the command: the buffer
    // is lexically importless, so the -include gate must trigger the
    // precise scan that records the name.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("deps.h", "import m;\n");
    tmp.touch("main.cpp", "int main() { return 0; }\n");
    auto src = tmp.path("main.cpp");

    Stack stack;
    write_cdb(tmp,
              stack.workspace.cdb,
              build_cdb_json({
                  {tmp.root, src, {"-include", tmp.path("deps.h")}}
    }));
    auto session = stack.open(src, "int main() { return 0; }\n");

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        CO_ASSERT_TRUE(stack.pool.start(opts));

        [[maybe_unused]] bool ok = co_await stack.ast.ensure_compiled(session);

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);

    auto dirtied = stack.graph.update(PCMFamily::unresolved_node("m"));
    EXPECT_TRUE(std::ranges::find(dirtied, NodeId{ast_family, session->path_id}) != dirtied.end());

    logging::reset_anomaly_for_testing();
}

};  // TEST_SUITE(ASTFamilyGuards)

TEST_SUITE(ForwarderGuards) {

TEST_CASE(QuarantineBlocksBuilds) {
    // A quarantined document gets no stateless builds either: completion
    // requests compile the same content the quarantine watches.
    Stack stack;
    auto session = stack.open("/proj/poison.cpp", "int x;\n");
    session->quarantine.on_crash();
    session->quarantine.on_crash();

    bool done = false;
    auto body = [&]() -> kota::task<> {
        auto result = co_await stack.forwarder.forward_completion({}, session);
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, worker::dispatch_errc::worker_unavailable);
        // The gate's message, not the empty pool's: without the gate this
        // test would still see worker_unavailable and prove nothing.
        EXPECT_TRUE(result.error().message.contains("quarantined"));
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);
}

TEST_CASE(QuarantineBlocksFormat) {
    // Formatting is still this document's content on a worker: quarantine
    // refuses it like any other stateless build.
    Stack stack;
    auto session = stack.open("/proj/poison.cpp", "int x;\n");
    session->quarantine.on_crash();
    session->quarantine.on_crash();

    bool done = false;
    auto body = [&]() -> kota::task<> {
        auto result = co_await stack.forwarder.forward_format(session, std::nullopt);
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, worker::dispatch_errc::worker_unavailable);
        EXPECT_TRUE(result.error().message.contains("quarantined"));
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);
}

TEST_CASE(EpochGuardsPCHWrite) {
    Stack stack;
    auto session = stack.open("/proj/a.cpp", "int x;");
    auto pid = session->path_id;
    // No preamble directives: a current request would take the pch_key
    // reset branch; an invalidated continuation must not touch it.
    stack.ast.projections.set_pch_key(pid, "key");

    auto gen = session->generation;
    auto epoch = stack.ast.projections.epoch(pid);
    // A Lost-type invalidation (disk/CDB change behind the in-flight
    // request) lands after takeoff: the projection epoch bumps,
    // generation stays.
    stack.ast.invalidate(pid);

    std::string directory = "/proj";
    std::vector<std::string> arguments = {"clang++", "-fsyntax-only", "/proj/a.cpp"};
    bool wrote = true;
    auto body = [&]() -> kota::task<> {
        wrote = co_await ForwarderFixture::ensure_pch(stack.forwarder,
                                                      session,
                                                      gen,
                                                      epoch,
                                                      directory,
                                                      arguments);
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();

    EXPECT_FALSE(wrote);
    // The stale continuation left the adopted PCH reference untouched.
    auto projection = stack.ast.projections.projection(pid);
    ASSERT_TRUE(projection && projection->pch_key.has_value());
    EXPECT_EQ(*projection->pch_key, std::string("key"));
}

TEST_CASE(PCHCrashCountsStreak) {
    // A PCH build that kills its stateless worker must count toward the
    // document's quarantine streak: the preamble is the document's content
    // too, and without this a poison preamble never quarantines.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    Stack stack;
    stack.register_pch_store(tmp);
    auto session = stack.open(src, "#pragma clang __debug crash\n");

    std::string directory = tmp.path(".");
    auto arguments = make_args(src);

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        CO_ASSERT_TRUE(stack.pool.start(opts));

        bool built =
            co_await ForwarderFixture::ensure_pch(stack.forwarder,
                                                  session,
                                                  session->generation,
                                                  stack.ast.projections.epoch(session->path_id),
                                                  directory,
                                                  arguments);
        EXPECT_FALSE(built);
        // Two strikes from one request: the retry's death is separate
        // evidence — blame is counted per worker killed, not per request.
        EXPECT_EQ(session->quarantine.crashes(), 2u);

        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);

    logging::reset_anomaly_for_testing();
}

TEST_CASE(PCHCrashBlocksBuild) {
    // A PCH crash inside a completion build's dependency prep can tip the
    // document into quarantine after the entry gate: the build must stop
    // instead of dispatching the same content to one more worker.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    Stack stack;
    stack.register_pch_store(tmp);
    auto session = stack.open(src, "#pragma clang __debug crash\n");
    session->quarantine.on_crash();

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        CO_ASSERT_TRUE(stack.pool.start(opts));

        auto result = co_await stack.forwarder.forward_completion({}, session);
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, worker::dispatch_errc::worker_unavailable);
        // One inherited strike plus both deaths of the doomed PCH build.
        EXPECT_EQ(session->quarantine.crashes(), 3u);

        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
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

    Stack stack;
    std::string text;
    text.reserve(1 << 21);
    for(int i = 0; i < 50'000; ++i) {
        text += std::format("int v{};\n", i);
    }
    auto session = stack.open(src, std::move(text));

    bool cancelled_returned = false;
    bool other_answered = false;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        CO_ASSERT_TRUE(stack.pool.start(opts));

        kota::cancellation_source source;
        kota::task_group<> group(stack.loop);
        auto cancelled_waiter = [&]() -> kota::task<> {
            auto hover = [&]() -> WorkerForwarder::RawResult {
                co_return co_await stack.forwarder.forward_query(worker::QueryKind::Hover,
                                                                 session,
                                                                 protocol::Position{0, 4},
                                                                 {},
                                                                 source.token());
            };
            auto r = co_await kota::with_token(hover(), source.token());
            cancelled_returned = r.is_cancelled();
        };
        auto other_waiter = [&]() -> kota::task<> {
            auto result = co_await stack.forwarder.forward_query(worker::QueryKind::Hover,
                                                                 session,
                                                                 protocol::Position{0, 4});
            other_answered = result.has_value();
        };
        group.spawn(cancelled_waiter());
        group.spawn(other_waiter());

        for(int i = 0; i < 100 && !stack.is_compiling(session->path_id); ++i) {
            co_await kota::sleep(10);
        }
        CO_ASSERT_TRUE(stack.is_compiling(session->path_id));
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
        EXPECT_TRUE(stack.ast.projections.current(session->path_id));
        EXPECT_FALSE(stack.is_compiling(session->path_id));

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
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

    Stack stack;
    stack.register_pch_store(tmp);

    auto make_session = [&] {
        auto session = std::make_shared<Session>();
        session->path_id = stack.workspace.path_pool.intern(src);
        session->text = "#pragma clang __debug crash\n";
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
        CO_ASSERT_TRUE(stack.pool.start(opts));

        auto build = [&](const std::shared_ptr<Session>& session) {
            return ForwarderFixture::ensure_pch(stack.forwarder,
                                                session,
                                                session->generation,
                                                stack.ast.projections.epoch(session->path_id),
                                                directory,
                                                arguments);
        };
        // One request, two dead workers, two strikes: the key blocks after
        // a single poison build instead of burning workers for a second
        // session's attempt.
        CO_ASSERT_FALSE(co_await build(first));
        EXPECT_EQ(first->quarantine.crashes(), 2u);

        // Refused without touching a worker.
        CO_ASSERT_FALSE(co_await build(second));
        EXPECT_EQ(second->quarantine.crashes(), 0u);
        CO_ASSERT_FALSE(co_await build(third));
        EXPECT_EQ(third->quarantine.crashes(), 0u);

        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);

    logging::reset_anomaly_for_testing();
}

TEST_CASE(EpochGuardsPCHWash) {
    // A successful build whose license epoch moved mid-flight must not
    // wash the session's PCH evidence: the strikes belong to content the
    // request no longer describes, and laundering them would let a poison
    // preamble dodge quarantine behind an old round's landing.
    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    Stack stack;
    stack.register_pch_store(tmp);
    auto session = stack.open(src, "#define X 1\nint x;\n");
    // One prior strike on the PCH ledger.
    session->quarantine.on_kind_crash(evidence_kind(EvidenceKind::PCH), "w-1");
    ASSERT_EQ(session->quarantine.crashes(), 1u);

    std::string directory = tmp.path(".");
    auto arguments = make_args(src);

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        CO_ASSERT_TRUE(stack.pool.start(opts));

        auto gen = session->generation;
        auto epoch = stack.ast.projections.epoch(session->path_id);
        bool built = true;
        auto launch = [&]() -> kota::task<> {
            built = co_await ForwarderFixture::ensure_pch(stack.forwarder,
                                                          session,
                                                          gen,
                                                          epoch,
                                                          directory,
                                                          arguments);
        };
        // Runs after the launch suspended on its dispatched build: a
        // Lost-type invalidation lands behind the in-flight request.
        auto invalidate = [&]() -> kota::task<> {
            stack.ast.invalidate(session->path_id);
            co_return;
        };
        co_await kota::when_all(launch(), invalidate());

        // The build landed (the shared artifact is cached), but the stale
        // request adopted nothing and washed nothing.
        EXPECT_FALSE(built);
        auto projection = stack.ast.projections.projection(session->path_id);
        EXPECT_TRUE(!projection || !projection->pch_key.has_value());
        EXPECT_EQ(session->quarantine.crashes(), 1u);

        // A current request adopts the cached pair and only then washes
        // this session's ledger.
        built = co_await ForwarderFixture::ensure_pch(stack.forwarder,
                                                      session,
                                                      session->generation,
                                                      stack.ast.projections.epoch(session->path_id),
                                                      directory,
                                                      arguments);
        EXPECT_TRUE(built);
        projection = stack.ast.projections.projection(session->path_id);
        EXPECT_TRUE(projection && projection->pch_key.has_value());
        EXPECT_EQ(session->quarantine.crashes(), 0u);

        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);
}

TEST_CASE(StaleDepsNoAdopt) {
    // Adoption is gated on the round outcome, never on leftover cache
    // paths: when the pair's deps went stale and the rebuild fails, every
    // waiter comes back empty-handed — nobody adopts the deps-stale pair
    // the cache still names.
    TempDir tmp;
    tmp.touch("a.cpp", "");
    tmp.touch("dep.h", "int dep();\n");
    auto src = tmp.path("a.cpp");

    Stack stack;
    stack.register_pch_store(tmp);

    auto make_session = [&] {
        auto session = std::make_shared<Session>();
        session->path_id = stack.workspace.path_pool.intern(src);
        session->text = "#include \"dep.h\"\nint x;\n";
        return session;
    };
    auto builder = make_session();
    auto first = make_session();
    auto second = make_session();

    std::string directory = tmp.path(".");
    auto arguments = make_args(src);

    auto build = [&](const std::shared_ptr<Session>& session) {
        return ForwarderFixture::ensure_pch(stack.forwarder,
                                            session,
                                            session->generation,
                                            stack.ast.projections.epoch(session->path_id),
                                            directory,
                                            arguments);
    };

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        CO_ASSERT_TRUE(stack.pool.start(opts));

        CO_ASSERT_TRUE(co_await build(builder));
        auto adopted = stack.ast.projections.projection(builder->path_id);
        CO_ASSERT_TRUE(adopted && adopted->pch_key.has_value());
        auto builder_key = *adopted->pch_key;

        // The header the pair depends on changes into one that cannot
        // compile: the pair is deps-stale and its rebuild fails.
        tmp.touch("dep.h", "#error dep changed\n");

        bool first_built = true;
        bool second_built = true;
        auto acquire_first = [&]() -> kota::task<> {
            first_built = co_await build(first);
        };
        auto acquire_second = [&]() -> kota::task<> {
            second_built = co_await build(second);
        };
        co_await kota::when_all(acquire_first(), acquire_second());

        EXPECT_FALSE(first_built);
        EXPECT_FALSE(second_built);
        // The projection keeps the builder's adoption — the failed rebuild
        // wrote nothing over it, and the stale pair heals on a later
        // successful acquisition.
        auto projection = stack.ast.projections.projection(first->path_id);
        EXPECT_TRUE(projection && projection->pch_key == builder_key);

        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    EXPECT_TRUE(done);
}

};  // TEST_SUITE(ForwarderGuards)

}  // namespace

}  // namespace clice::testing
