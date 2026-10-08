module;

#include "modules/prelude.h"

module clice;

import :sched.families.pch;
import :sched.families.pcm;
import :sched.graph;
import :server.ast_family;
import :server.dispatcher;
import :server.editor_context;
import :support.anomaly;
import :syntax.dependency_graph;
import :tests.unit.server.worker_test_helpers;
import :tests.unit.test.cdb_helper;
import :tests.unit.test.temp_dir;
import :tests.unit.test.test;
import :vfs.cache_store;

namespace clice::testing {

/// Reaches the family's private PCH adoption step for guard tests: whether
/// a request on the session's buffer compiles against a PCH.
struct ASTFamilyFixture {
    static kota::task<bool> ensure_pch(ASTFamily& ast,
                                       const std::shared_ptr<Session>& session,
                                       std::uint64_t license_generation,
                                       std::uint64_t license_epoch,
                                       const std::string& directory,
                                       const std::vector<std::string>& arguments) {
        auto key = co_await ast.ensure_pch(session,
                                           session->text,
                                           license_generation,
                                           license_epoch,
                                           directory,
                                           arguments,
                                           nullptr,
                                           {});
        co_return key.has_value();
    }
};

namespace {

/// The full server-side compile stack, workerless until a test starts the
/// pool. Sessions are opened through the store so the family's runner
/// resolves them.
struct Stack {
    kota::event_loop loop;
    FileTable files;
    Project project{files};
    CommandResolver commands{project};
    ContextsBlob blob;
    EditorContext contexts{project, commands, blob};
    WorkerPool pool{loop};
    TaskGraph graph;
    PCMFamily pcm{graph, project, commands, pool};
    PCHFamily pch{graph, project, pool};
    SessionStore sessions;
    ASTFamily ast{project, contexts, graph, pcm, pch, pool, sessions};
    Dispatcher dispatcher{project, contexts, ast, pool};

    Stack() {
        pcm.register_runner();
        pch.register_runner();
        ast.register_runner();
    }

    std::shared_ptr<Session> open(llvm::StringRef path, std::string text) {
        auto session = sessions.open(project.file_table.intern(Spelling::absolute(path)));
        session->text = std::move(text);
        session->sync_text();
        return session;
    }

    bool is_compiling(Fid path_id) const {
        return graph.is_compiling({Family::AST, path_id.raw});
    }

    void register_pch_store(TempDir& tmp) {
        auto store = CacheStore::open(tmp.path("root"), 1);
        ZASSERT(store);
        store->register_namespace({.name = "pch",
                                   .extension = ".pch",
                                   .aux_extension = ".pch.idx",
                                   .policy = CachePolicy::LRU,
                                   .max_bytes = 1ull << 30});
        project.store.emplace(std::move(*store));
    }
};

ZEST_SUITE(ASTFamilyGuards) {

ZEST_CASE(SupersedeTouchesEntry) {
    Stack stack;
    auto session = stack.open("/proj/a.cpp", "int x;\n");
    auto pid = session->path_id;
    stack.ast.projections.entries[pid].current = true;
    auto epoch = stack.ast.projections.epoch(pid);

    stack.ast.supersede(pid);

    ZASSERT(!stack.ast.projections.current(pid));
    ZASSERT(stack.ast.projections.epoch(pid) == epoch + 1);
}

ZEST_CASE(InvalidateKeepsProjection) {
    Stack stack;
    auto session = stack.open("/proj/a.cpp", "int x;\n");
    auto pid = session->path_id;
    stack.ast.projections.set_pch_key(pid, "key");
    stack.ast.projections.entries[pid].current = true;

    stack.ast.invalidate(pid);

    // Only the currency claim is revoked; the products stay for the
    // bounded-staleness consumers (preamble links, hover gap).
    ZASSERT(!stack.ast.projections.current(pid));
    auto projection = stack.ast.projections.projection(pid);
    ZASSERT(projection != nullptr);
    ZASSERT(projection->pch_key);
}

ZEST_CASE(DropErasesEntry) {
    Stack stack;
    auto session = stack.open("/proj/a.cpp", "int x;\n");
    auto pid = session->path_id;
    stack.ast.projections.set_pch_key(pid, "key");

    stack.ast.drop(pid);

    ZASSERT(stack.ast.projections.projection(pid) == nullptr);
    ZASSERT(!stack.ast.projections.current(pid));
}

ZEST_CASE(SwitchIdentityResets) {
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
    ZASSERT(session->generation == generation + 1);
    ZASSERT(!session->trial_done);
    ZASSERT(!stack.ast.projections.current(pid));
    ZASSERT(!stack.ast.projections.entries[pid].deps.has_value());
    auto after = stack.ast.projections.projection(pid);
    ZASSERT(!after->pch_key.has_value());
    ZASSERT(after->output);
}

ZEST_CASE(CrashPublishesNote) {
    // A recorded compile crash publishes its note at once — the bar that
    // follows publishes nothing more — and takes the old diagnostics down:
    // no AST stands behind them anymore.
    Stack stack;
    auto session = stack.open("/proj/poison.cpp", "int x;\n");
    stack.ast.projections.set_output(
        session->path_id,
        CompileOutput{.version = 1,
                      .source = CommandSource::CDBExact,
                      .diagnostics = {protocol::Diagnostic{.message = "old"}}});

    int emits = 0;
    auto conn = stack.ast.on_output.connect([&](const std::shared_ptr<Session>&) { emits += 1; });
    stack.ast.record_crash(
        session,
        evidence_kind(EvidenceKind::Compile),
        kota::ipc::Error{worker::dispatch_errc::worker_crashed, "killed by signal 11 (SIGSEGV)"});
    ZEXPECT(emits == 1);
    auto projection = stack.ast.projections.projection(session->path_id);
    ZASSERT((projection && projection->output.has_value()));
    ZEXPECT(projection->output->diagnostics.empty());

    std::vector<protocol::Diagnostic> notes;
    append_crash_notes(*session, notes);
    ZASSERT(notes.size() == 1u);
    auto& message = std::get<std::string>(notes[0].message);
    ZEXPECT(message.contains("while compiling this file (killed by signal 11 (SIGSEGV))"));
    ZEXPECT(message.contains("save it"));

    bool done = false;
    auto body = [&]() -> kota::task<> {
        ZASSERT(!co_await stack.ast.ensure_compiled(session));
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);
    ZEXPECT(emits == 1);
}

ZEST_CASE(ShutdownUnblocksWaiters) {
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
        ZASSERT(stack.pool.start(opts));

        kota::task_group<> group;
        auto waiter = [&]() -> kota::task<> {
            waiter_ok = co_await stack.ast.ensure_compiled(session);
            waiter_done = true;
        };
        group.spawn(waiter());

        for(int i = 0; i < 100 && !stack.is_compiling(session->path_id); ++i) {
            co_await kota::sleep(10);
        }
        ZASSERT(stack.is_compiling(session->path_id));
        ZASSERT(!waiter_done);

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

        ZEXPECT(waiter_done);
        ZEXPECT(!waiter_ok);
        ZEXPECT(!stack.is_compiling(session->path_id));

        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(EditInterruptsStaleCompile) {
    // The didChange path: an edit with NO follow-up request supersedes the
    // in-flight round — cancelling its compile request makes the worker
    // abandon the stale parse, the waiter resolves false (its result is
    // for a buffer that no longer exists — the editor re-requests after
    // an edit), and the next request compiles the fresh content. Liveness
    // pin; the interruption content is pinned by
    // StatefulWorker.WireCancelInterruptsCompile.
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
        ZASSERT(stack.pool.start(opts));

        kota::task_group<> group;
        auto waiter = [&]() -> kota::task<> {
            waiter_ok = co_await stack.ast.ensure_compiled(session);
            waiter_done = true;
        };
        group.spawn(waiter());

        for(int i = 0; i < 100 && !stack.is_compiling(session->path_id); ++i) {
            co_await kota::sleep(10);
        }
        ZASSERT(stack.is_compiling(session->path_id));

        // What the didChange handler does: fold the edit in, then supersede.
        session->text = "int fixed;\n";
        session->sync_text();
        session->generation += 1;
        stack.ast.supersede(session->path_id);

        for(int i = 0; i < 600 && !waiter_done; ++i) {
            co_await kota::sleep(100);
        }
        if(!waiter_done) {
            group.cancel();
        }
        co_await group.join();

        ZASSERT(waiter_done);
        ZEXPECT(!waiter_ok);

        // The next request (the editor re-queries after an edit) compiles
        // the fresh content.
        bool second_ok = co_await stack.ast.ensure_compiled(session);
        ZEXPECT(second_ok);
        ZEXPECT(stack.ast.projections.current(session->path_id));

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(SupersededCompileCancelled) {
    // An edit mid-compile supersedes the in-flight round: the first waiter
    // abandons (its buffer is gone), the supersede point interrupts the
    // worker's parse, and the second waiter's respawned round compiles the
    // new content. This pins the supersede path's liveness; the
    // interruption itself is pinned content-wise by
    // StatefulWorker.WireCancelInterruptsCompile.
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
        ZASSERT(stack.pool.start(opts));

        kota::task_group<> group;
        auto first = [&]() -> kota::task<> {
            [[maybe_unused]] bool ok = co_await stack.ast.ensure_compiled(session);
            first_done = true;
        };
        group.spawn(first());

        for(int i = 0; i < 100 && !stack.is_compiling(session->path_id); ++i) {
            co_await kota::sleep(10);
        }
        ZASSERT(stack.is_compiling(session->path_id));

        // The edit lands while the slow compile is in flight.
        session->text = "int fixed;\n";
        session->sync_text();
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

        ZEXPECT(first_done);
        ZEXPECT(second_ok);
        ZEXPECT(stack.ast.projections.current(session->path_id));

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(BufferImportBuildsPCM) {
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
              stack.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"), {}},
                  {tmp.root, src,                {}},
    }));
    scan_all(stack.project.cdb, stack.project.dep_graph);
    stack.project.dep_graph.build_reverse_map();

    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace({.name = "pch",
                               .extension = ".pch",
                               .aux_extension = ".pch.idx",
                               .policy = CachePolicy::LRU,
                               .max_bytes = 1ull << 30});
    store->register_namespace(
        {.name = "pcm", .extension = ".pcm", .policy = CachePolicy::LRU, .max_bytes = 1ull << 30});
    stack.project.store.emplace(std::move(*store));

    auto session = stack.open(src, "import m;\nint main() { return mv(); }\n");

    bool ok = false;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 1;
        ZASSERT(stack.pool.start(opts));

        ok = co_await stack.ast.ensure_compiled(session);

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    ZEXPECT(ok);
    ZEXPECT(stack.ast.projections.current(session->path_id));
    auto mod_ids = stack.project.dep_graph.lookup_module("m");
    ZASSERT(!mod_ids.empty());
    ZEXPECT(stack.project.pcm_cache.contains(mod_ids[0]));
}

ZEST_CASE(DiamondDepsDeduplicated) {
    // top reaches base through left and through right: its PCM's inputs
    // name base's source once.
    TempDir tmp;
    tmp.touch("base.cppm", "export module base;\nexport int b() { return 1; }\n");
    tmp.touch("left.cppm", "export module left;\nimport base;\nexport int l() { return b(); }\n");
    tmp.touch("right.cppm", "export module right;\nimport base;\nexport int r() { return b(); }\n");
    tmp.touch("top.cppm",
              "export module top;\nimport left;\nimport right;\n"
              "export int t() { return l() + r(); }\n");
    tmp.touch("main.cpp", "int main() { return 0; }\n");
    auto src = tmp.path("main.cpp");

    Stack stack;
    write_cdb(tmp,
              stack.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("base.cppm"),  {}},
                  {tmp.root, tmp.path("left.cppm"),  {}},
                  {tmp.root, tmp.path("right.cppm"), {}},
                  {tmp.root, tmp.path("top.cppm"),   {}},
                  {tmp.root, src,                    {}},
    }));
    scan_all(stack.project.cdb, stack.project.dep_graph);
    stack.project.dep_graph.build_reverse_map();

    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace({.name = "pch",
                               .extension = ".pch",
                               .aux_extension = ".pch.idx",
                               .policy = CachePolicy::LRU,
                               .max_bytes = 1ull << 30});
    store->register_namespace(
        {.name = "pcm", .extension = ".pcm", .policy = CachePolicy::LRU, .max_bytes = 1ull << 30});
    stack.project.store.emplace(std::move(*store));

    auto session = stack.open(src, "import top;\nint main() { return t(); }\n");

    bool ok = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 1;
        ZASSERT(stack.pool.start(opts));

        ok = co_await stack.ast.ensure_compiled(session);

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZASSERT(ok);

    auto top = stack.project.dep_graph.lookup_module("top");
    ZASSERT(!top.empty());
    auto entry = stack.project.pcm_cache.find(top[0]);
    ZASSERT(entry != stack.project.pcm_cache.end());
    auto base = stack.project.file_table.intern(Spelling::absolute(tmp.path("base.cppm")));
    auto& deps = entry->second.deps;
    ZEXPECT(llvm::count_if(deps, [&](const DepState& dep) { return dep.path_id == base; }) == 1);
}

ZEST_CASE(ImportScanPerUnit) {
    // In a project with modules only a unit that can import pays the
    // precise scan, and an edit off its directive lines reuses it.
    TempDir tmp;
    tmp.touch("m.cppm",
              "export module m;\n"
              "export int mv() { return 1; }\n");
    tmp.touch("main.cpp", "import m;\nint main() { return mv(); }\n");
    tmp.touch("plain.cpp", "int plain() { return 0; }\n");

    Stack stack;
    write_cdb(tmp,
              stack.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"),    {}},
                  {tmp.root, tmp.path("main.cpp"),  {}},
                  {tmp.root, tmp.path("plain.cpp"), {}},
    }));
    scan_all(stack.project.cdb, stack.project.dep_graph);
    stack.project.dep_graph.build_reverse_map();
    stack.register_pch_store(tmp);
    stack.project.store->register_namespace(
        {.name = "pcm", .extension = ".pcm", .policy = CachePolicy::LRU, .max_bytes = 1ull << 30});

    auto plain = stack.open(tmp.path("plain.cpp"), "int plain() { return 0; }\n");
    auto main = stack.open(tmp.path("main.cpp"), "import m;\nint main() { return mv(); }\n");
    auto edit = [&](const std::shared_ptr<Session>& session, std::string text) {
        session->text = std::move(text);
        session->sync_text();
        session->generation += 1;
        stack.ast.supersede(session->path_id);
    };

    std::uint64_t plain_scans = 0;
    std::uint64_t first_scans = 0;
    std::uint64_t body_scans = 0;
    std::uint64_t import_scans = 0;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 1;
        ZASSERT(stack.pool.start(opts));

        ZASSERT(co_await stack.ast.ensure_compiled(plain));
        plain_scans = stack.pcm.import_scans;
        ZASSERT(co_await stack.ast.ensure_compiled(main));
        first_scans = stack.pcm.import_scans;

        edit(main, "import m;\nint main() { return mv() + 1; }\n");
        ZASSERT(co_await stack.ast.ensure_compiled(main));
        body_scans = stack.pcm.import_scans;

        edit(main, "import m;\n#define TWO 2\nint main() { return mv() + TWO; }\n");
        ZASSERT(co_await stack.ast.ensure_compiled(main));
        import_scans = stack.pcm.import_scans;

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    ZEXPECT(plain_scans == 0u);
    // The document's scan, and the interface's own PCM round.
    ZEXPECT(first_scans == 2u);
    ZEXPECT(body_scans == first_scans);
    ZEXPECT(import_scans == first_scans + 1);
}

ZEST_CASE(BufferImportRecorded) {
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
        ZASSERT(stack.pool.start(opts));

        [[maybe_unused]] bool ok = co_await stack.ast.ensure_compiled(session);

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    // The sentinel edge survives the landing: a first provider's update
    // must reach this document's node.
    auto dirtied = stack.graph.update(PCMFamily::unresolved_node("m"));
    ZEXPECT(std::ranges::find(dirtied, NodeId{Family::AST, session->path_id.raw}) != dirtied.end());

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(IncludeImportRecorded) {
    // Zero-provider window, import introduced by the command: the buffer
    // is lexically importless, so the forced header's import syntax, a
    // graph fact, must trigger the precise scan that records the name.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("deps.h", "import m;\n");
    tmp.touch("main.cpp", "int main() { return 0; }\n");
    auto src = tmp.path("main.cpp");

    Stack stack;
    write_cdb(tmp,
              stack.project.cdb,
              build_cdb_json({
                  {tmp.root, src, {"-include", tmp.path("deps.h")}}
    }));
    scan_all(stack.project.cdb, stack.project.dep_graph);
    stack.project.dep_graph.build_reverse_map();
    auto session = stack.open(src, "int main() { return 0; }\n");

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(stack.pool.start(opts));

        [[maybe_unused]] bool ok = co_await stack.ast.ensure_compiled(session);

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    auto dirtied = stack.graph.update(PCMFamily::unresolved_node("m"));
    ZEXPECT(std::ranges::find(dirtied, NodeId{Family::AST, session->path_id.raw}) != dirtied.end());

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(HostImportRecorded) {
    // A header compiled through its host's synthesized prefix: the host's
    // import syntax, a graph fact, must trigger the precise scan that
    // records the name.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("part.inc", "int part();\n");
    tmp.touch("main.cpp", "import m;\n#include \"part.inc\"\nint main() { return 0; }\n");

    Stack stack;
    write_cdb(tmp,
              stack.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    scan_all(stack.project.cdb, stack.project.dep_graph);
    stack.project.dep_graph.build_reverse_map();
    auto session = stack.open(tmp.path("part.inc"), "int part();\n");

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(stack.pool.start(opts));

        [[maybe_unused]] bool ok = co_await stack.ast.ensure_compiled(session);

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    auto* context = stack.contexts.header_context(session->path_id);
    ZASSERT((context != nullptr && context->synthesized != nullptr));
    auto dirtied = stack.graph.update(PCMFamily::unresolved_node("m"));
    ZEXPECT(std::ranges::find(dirtied, NodeId{Family::AST, session->path_id.raw}) != dirtied.end());

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(ForcedIncludeSkipsScan) {
    // Neither the command's forced header nor a header compiled through
    // its host has import syntax: no compile pays an import scan.
    TempDir tmp;
    tmp.touch("force.h", "#define FORCED 1\n");
    tmp.touch("header.h", "inline int header() { return FORCED; }\n");
    tmp.touch("main.cpp", "#include \"header.h\"\nint main() { return header(); }\n");
    auto src = tmp.path("main.cpp");

    Stack stack;
    write_cdb(tmp,
              stack.project.cdb,
              build_cdb_json({
                  {tmp.root, src, {"-include", "force.h"}}
    }));
    scan_all(stack.project.cdb, stack.project.dep_graph);
    stack.project.dep_graph.build_reverse_map();
    auto unit = stack.open(src, "#include \"header.h\"\nint main() { return header(); }\n");
    auto header = stack.open(tmp.path("header.h"), "inline int header() { return FORCED; }\n");

    bool unit_ok = false;
    bool header_ok = false;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(stack.pool.start(opts));

        unit_ok = co_await stack.ast.ensure_compiled(unit);
        header_ok = co_await stack.ast.ensure_compiled(header);

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    ZEXPECT(unit_ok);
    ZEXPECT(header_ok);
    ZEXPECT(stack.contexts.header_context(header->path_id) != nullptr);
    ZEXPECT(stack.pcm.import_scans == 0u);
}

};  // ZEST_SUITE(ASTFamilyGuards)

ZEST_SUITE(DispatcherGuards) {

ZEST_CASE(CrashedCompileBarsBuilds) {
    // A barred compile bars the stateless builds of the same content too —
    // completion parses what the compile crashed on — and they answer
    // empty: the crash note says why, an error would only be a popup.
    Stack stack;
    auto session = stack.open("/proj/poison.cpp", "int x;\n");
    session->quarantine->on_crash(evidence_kind(EvidenceKind::Compile),
                                  "d1",
                                  "cause",
                                  Quarantine::Clock::now());

    bool done = false;
    auto body = [&]() -> kota::task<> {
        // With no worker at all, only the bar can answer without an error.
        auto result = co_await stack.dispatcher.completion(Ticket::take(session), {}, {});
        ZASSERT(result);
        ZEXPECT(result.value().data == "null");
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);
}

ZEST_CASE(CrashedFormatBarsFormat) {
    Stack stack;
    auto session = stack.open("/proj/poison.cpp", "int x;\n");
    session->quarantine->on_crash(evidence_kind(EvidenceKind::Format),
                                  "d1",
                                  "cause",
                                  Quarantine::Clock::now());

    bool done = false;
    auto body = [&]() -> kota::task<> {
        auto result = co_await stack.dispatcher.format(Ticket::take(session), std::nullopt);
        ZASSERT(result);
        ZEXPECT(result.value().data == "null");
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);
}

ZEST_CASE(EpochGuardsPCHWrite) {
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
        wrote = co_await ASTFamilyFixture::ensure_pch(stack.ast,
                                                      session,
                                                      gen,
                                                      epoch,
                                                      directory,
                                                      arguments);
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();

    ZEXPECT(!wrote);
    // The stale continuation left the adopted PCH reference untouched.
    auto projection = stack.ast.projections.projection(pid);
    ZASSERT((projection && projection->pch_key.has_value()));
    ZEXPECT(*projection->pch_key == std::string("key"));
}

ZEST_CASE(PCHCrashBarsDocument) {
    // A PCH build that kills its stateless worker bars the document: the
    // preamble is its content too. The worker names the build, so the
    // death is blamed at once and the build is not resent.
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
        ZASSERT(stack.pool.start(opts));

        bool built =
            co_await ASTFamilyFixture::ensure_pch(stack.ast,
                                                  session,
                                                  session->generation,
                                                  stack.ast.projections.epoch(session->path_id),
                                                  directory,
                                                  arguments);
        ZEXPECT(!built);
        ZEXPECT(ASTFamily::compile_barred(*session));
        auto notes = session->quarantine->notes();
        ZASSERT(notes.size() == 1u);
        ZEXPECT(notes[0].kind == evidence_kind(EvidenceKind::PCH));
        ZEXPECT(notes[0].strikes == 1u);

        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(PCHCrashStopsBuild) {
    // A PCH crash inside a completion build's dependency prep bars the
    // document after the entry gate: the build stops instead of
    // dispatching the same content to one more worker.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    Stack stack;
    stack.register_pch_store(tmp);
    auto session = stack.open(src, "#pragma clang __debug crash\n");

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(stack.pool.start(opts));

        auto result = co_await stack.dispatcher.completion(Ticket::take(session), {}, {});
        ZASSERT(result);
        ZEXPECT(result.value().data == "null");
        auto notes = session->quarantine->notes();
        ZASSERT(notes.size() == 1u);
        ZEXPECT(notes[0].strikes == 1u);

        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(ClientCancelSparesCompile) {
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
        ZASSERT(stack.pool.start(opts));

        kota::cancellation_source source;
        kota::task_group<> group;
        auto cancelled_waiter = [&]() -> kota::task<> {
            auto hover = [&]() -> Dispatcher::RawResult {
                co_return co_await stack.dispatcher.query(worker::QueryKind::Hover,
                                                          Ticket::take(session),
                                                          protocol::Position{0, 4},
                                                          {},
                                                          source.token());
            };
            auto r = co_await kota::with_token(hover(), source.token());
            cancelled_returned = r.is_cancelled();
        };
        auto other_waiter = [&]() -> kota::task<> {
            auto result = co_await stack.dispatcher.query(worker::QueryKind::Hover,
                                                          Ticket::take(session),
                                                          protocol::Position{0, 4});
            other_answered = result.has_value();
        };
        group.spawn(cancelled_waiter());
        group.spawn(other_waiter());

        for(int i = 0; i < 100 && !stack.is_compiling(session->path_id); ++i) {
            co_await kota::sleep(10);
        }
        ZASSERT(stack.is_compiling(session->path_id));
        source.cancel();

        for(int i = 0; i < 600 && !other_answered; ++i) {
            co_await kota::sleep(100);
        }
        if(!other_answered) {
            group.cancel();
        }
        co_await group.join();

        ZEXPECT(cancelled_returned);
        ZEXPECT(other_answered);
        ZEXPECT(stack.ast.projections.current(session->path_id));
        ZEXPECT(!stack.is_compiling(session->path_id));

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(StaleReplyLandsContentModified) {
    // A reply for a buffer the client edited away never leaves as a value:
    // the landing turns it into ContentModified even when the worker
    // answered after the compile had settled — the edit landed while the
    // query itself was in flight.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("stale.cpp", "");
    auto src = tmp.path("stale.cpp");

    Stack stack;
    std::string text;
    text.reserve(1 << 20);
    for(int i = 0; i < 50'000; i += 1) {
        text += std::format("int v{};\n", i);
    }
    auto session = stack.open(src, std::move(text));

    bool stale = false;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(stack.pool.start(opts));

        // Warm the AST: the next query's only suspension is its worker send.
        auto warm = co_await stack.dispatcher.query(worker::QueryKind::Hover,
                                                    Ticket::take(session),
                                                    protocol::Position{0, 4});
        ZASSERT(warm);

        auto ticket = Ticket::take(session);
        kota::task_group<> group;
        auto tokens = [&]() -> kota::task<> {
            auto result =
                co_await stack.dispatcher.query(worker::QueryKind::SemanticTokens, ticket);
            stale = !result.has_value() && result.error().code == content_modified_code;
        };
        group.spawn(tokens());
        // spawn runs the query inline to that suspension, so the pool
        // already reports it in flight: the bump lands provably between
        // dispatch and reply.
        ZASSERT(stack.pool.foreground_busy());
        session->generation += 1;
        co_await group.join();
        ZEXPECT(stale);

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(HarmlessKindKeepsBar) {
    // A crash of one query kind bars only that kind: a hover on a document
    // whose semantic tokens crashed is ordinary work — it is answered, and
    // the tokens record stays as it was.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    Stack stack;
    auto session = stack.open(src, "int x;\n");
    constexpr auto tokens_kind = evidence_kind(worker::QueryKind::SemanticTokens);
    session->quarantine->on_crash(tokens_kind, "w-1", "cause", Quarantine::Clock::now());

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(stack.pool.start(opts));

        auto result = co_await stack.dispatcher.query(worker::QueryKind::Hover,
                                                      Ticket::take(session),
                                                      protocol::Position{0, 4});
        ZEXPECT(result);
        ZEXPECT(session->quarantine->barred(tokens_kind, Quarantine::Clock::now()));

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(PoisonPreambleShared) {
    // One document's quarantine cannot contain a poison preamble alone: the
    // PCH is shared, so every session with the same preamble would
    // re-trigger the build and burn a worker of its own. After one crashed
    // build the key is refused before any dispatch, and every other session
    // books the same death instead of a fresh one.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    Stack stack;
    stack.register_pch_store(tmp);

    auto make_session = [&] {
        auto session = std::make_shared<Session>();
        session->path_id = stack.project.file_table.intern(Spelling::absolute(src));
        session->text = "#pragma clang __debug crash\n";
        return session;
    };
    auto first = make_session();
    auto second = make_session();
    auto third = make_session();

    std::string directory = tmp.path(".");
    auto arguments = make_args(src);

    int deaths = 0;
    stack.pool.on_crash = [&](const WorkerCrashInfo&) {
        deaths += 1;
    };

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(stack.pool.start(opts));

        auto build = [&](const std::shared_ptr<Session>& session) {
            return ASTFamilyFixture::ensure_pch(stack.ast,
                                                session,
                                                session->generation,
                                                stack.ast.projections.epoch(session->path_id),
                                                directory,
                                                arguments);
        };
        auto pch = evidence_kind(EvidenceKind::PCH);
        ZASSERT(!co_await build(first));
        ZASSERT(first->quarantine->crashed(pch));
        ZEXPECT(deaths == 1);

        // Refused without touching a worker, each session barred by the
        // same death.
        ZASSERT(!co_await build(second));
        ZEXPECT(second->quarantine->crashed(pch));
        ZASSERT(!co_await build(third));
        ZEXPECT(third->quarantine->crashed(pch));
        ZEXPECT(deaths == 1);

        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(EpochGuardsPCHWash) {
    // A successful build whose license epoch moved mid-flight must not
    // clear the session's PCH record: the crash belongs to content the
    // request no longer describes, and clearing it would let a poison
    // preamble dodge its bar behind an old round's landing.
    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    Stack stack;
    stack.register_pch_store(tmp);
    auto session = stack.open(src, "#define X 1\nint x;\n");
    // One prior crash on the PCH record.
    auto pch = evidence_kind(EvidenceKind::PCH);
    session->quarantine->on_crash(pch, "w-1", "cause", Quarantine::Clock::now());

    std::string directory = tmp.path(".");
    auto arguments = make_args(src);

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(stack.pool.start(opts));

        auto gen = session->generation;
        auto epoch = stack.ast.projections.epoch(session->path_id);
        bool built = true;
        auto launch = [&]() -> kota::task<> {
            built = co_await ASTFamilyFixture::ensure_pch(stack.ast,
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

        // The build landed (the shared artifact is cached) and the stale
        // request compiles against it, but it adopted nothing and cleared
        // nothing.
        ZEXPECT(built);
        auto projection = stack.ast.projections.projection(session->path_id);
        ZEXPECT((!projection || !projection->pch_key.has_value()));
        ZEXPECT(session->quarantine->crashed(pch));

        // A current request adopts the cached pair and only then clears
        // this session's record.
        built = co_await ASTFamilyFixture::ensure_pch(stack.ast,
                                                      session,
                                                      session->generation,
                                                      stack.ast.projections.epoch(session->path_id),
                                                      directory,
                                                      arguments);
        ZEXPECT(built);
        projection = stack.ast.projections.projection(session->path_id);
        ZEXPECT((projection && projection->pch_key.has_value()));
        ZEXPECT(!session->quarantine->crashed(pch));

        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);
}

ZEST_CASE(StaleDepsNoAdopt) {
    // Adoption is gated on the round outcome, never on leftover cache
    // paths: when the pair's deps went stale and the rebuild fails, every
    // waiter comes back empty-handed — nobody adopts the deps-stale pair
    // the cache still names.
    TempDir tmp;
    tmp.touch("a.cpp", "");
    tmp.touch("dep.h", "int dep();\n");
    tmp.touch("broken.pcm", "not a module file");
    auto src = tmp.path("a.cpp");

    Stack stack;
    stack.register_pch_store(tmp);

    auto make_session = [&] {
        auto session = std::make_shared<Session>();
        session->path_id = stack.project.file_table.intern(Spelling::absolute(src));
        session->text = "#include \"dep.h\"\nint x;\n";
        return session;
    };
    auto builder = make_session();
    auto first = make_session();
    auto second = make_session();

    std::string directory = tmp.path(".");
    auto arguments = make_args(src, "-std=c++20");
    arguments.insert(arguments.begin() + 1, "-fmodule-file=broken=" + tmp.path("broken.pcm"));

    auto build = [&](const std::shared_ptr<Session>& session) {
        return ASTFamilyFixture::ensure_pch(stack.ast,
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
        ZASSERT(stack.pool.start(opts));

        ZASSERT(co_await build(builder));
        auto adopted = stack.ast.projections.projection(builder->path_id);
        ZASSERT((adopted && adopted->pch_key.has_value()));
        auto builder_key = *adopted->pch_key;

        // The header the pair depends on changes into one importing a
        // module that cannot load: the pair is deps-stale and its rebuild
        // fails, as no PCH is written past a failed module load.
        tmp.touch("dep.h", "import broken;\n");

        bool first_built = true;
        bool second_built = true;
        auto acquire_first = [&]() -> kota::task<> {
            first_built = co_await build(first);
        };
        auto acquire_second = [&]() -> kota::task<> {
            second_built = co_await build(second);
        };
        co_await kota::when_all(acquire_first(), acquire_second());

        ZEXPECT(!first_built);
        ZEXPECT(!second_built);
        // The projection keeps the builder's adoption — the failed rebuild
        // wrote nothing over it, and the stale pair heals on a later
        // successful acquisition.
        auto projection = stack.ast.projections.projection(first->path_id);
        ZEXPECT((projection && projection->pch_key == builder_key));

        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);
}

ZEST_CASE(EvictedDocumentRecompiles) {
    // The worker evicts a document behind the master's back (no eviction
    // notice is wired here): the query hears document_unloaded, compiles it
    // there again and answers, never null.
    TempDir tmp;
    tmp.touch("a.cpp", "");
    tmp.touch("b.cpp", "");

    Stack stack;
    auto a = stack.open(tmp.path("a.cpp"), "int alpha = 1;\n");
    auto b = stack.open(tmp.path("b.cpp"), "int beta = 2;\n");

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        opts.max_documents = 1;
        ZASSERT(stack.pool.start(opts));

        ZASSERT(co_await stack.ast.ensure_compiled(a));
        ZASSERT(co_await stack.ast.ensure_compiled(b));
        auto result = co_await stack.dispatcher.query(worker::QueryKind::Hover,
                                                      Ticket::take(a),
                                                      protocol::Position{0, 5});
        ZASSERT(result);
        ZEXPECT(result.value().data != "null");

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);
}

ZEST_CASE(EvictedAgainRecompiles) {
    // The worker loses the document again between the recompile's reply and
    // the query — another document's request landing in that window: the
    // query compiles it there again for every eviction it meets.
    TempDir tmp;
    tmp.touch("a.cpp", "");

    Stack stack;
    auto a = stack.open(tmp.path("a.cpp"), "int alpha = 1;\n");
    auto evict = [&] {
        stack.pool.notify_stateful(
            a->path_id.raw,
            worker::EvictParams{std::string(stack.project.file_table.resolve(a->path_id))});
    };
    int landings = 0;
    auto connection = stack.ast.on_output.connect([&](const std::shared_ptr<Session>&) {
        landings += 1;
        if(landings == 2) {
            evict();
        }
    });

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(stack.pool.start(opts));

        ZASSERT(co_await stack.ast.ensure_compiled(a));
        evict();
        auto result = co_await stack.dispatcher.query(worker::QueryKind::Hover,
                                                      Ticket::take(a),
                                                      protocol::Position{0, 5});
        ZASSERT(result);
        ZEXPECT(result.value().data != "null");
        ZEXPECT(landings == 3);

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);
}

ZEST_CASE(AnswerClearsQueryRecord) {
    // Only an answer of the kind clears its record, and no compile runs to
    // drop the note: the answer republishes.
    TempDir tmp;
    tmp.touch("a.cpp", "");

    Stack stack;
    auto a = stack.open(tmp.path("a.cpp"), "int alpha = 1;\n");
    auto hover = evidence_kind(worker::QueryKind::Hover);
    a->quarantine->on_crash(hover, "d1", "cause", Quarantine::Clock::now());
    a->quarantine->on_save();
    int published = 0;
    auto connection =
        stack.ast.on_output.connect([&](const std::shared_ptr<Session>&) { published += 1; });

    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(stack.pool.start(opts));

        ZASSERT(co_await stack.ast.ensure_compiled(a));
        auto before = published;
        auto result = co_await stack.dispatcher.query(worker::QueryKind::Hover,
                                                      Ticket::take(a),
                                                      protocol::Position{0, 5});
        ZASSERT(result);
        ZEXPECT(result.value().data != "null");
        ZEXPECT(!a->quarantine->crashed(hover));
        ZEXPECT(published == before + 1);

        co_await stack.ast.stop();
        co_await stack.graph.shutdown();
        co_await stack.pool.stop();
        done = true;
    };
    auto task = body();
    stack.loop.schedule(task);
    stack.loop.run();
    ZEXPECT(done);
}

};  // ZEST_SUITE(DispatcherGuards)

}  // namespace

}  // namespace clice::testing
