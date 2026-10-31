module clice;

import :feature.diagnostics;
import :feature.hover;
import :project.hosting;
import :sched.builders.pcm;
import :sched.graph;
import :server.master;
import :server.served_project;
import :server.session;
import :support.anomaly;
import :syntax.dependency_graph;
import :syntax.position;
import :unittests.server.served_project_fixture;
import :unittests.server.worker_test_helpers;
import :unittests.utils;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :vfs.cache_store;
import :vfs.file_table;
import :worker.pool;
import :worker.probe;
import :worker.protocol;

namespace clice::testing {

namespace {

/// Park the work of the next compile of `path_id` where it starts in the
/// worker (see BuildProbe::gate); returns the gate's id.
std::uint64_t gate_compile(ServedProjectFixture& fixture, Fid path_id) {
    auto& server = fixture.server;
    server.pool.probe = &server.probe;
    worker::CompileParams params;
    params.path = std::string(server.files.resolve(path_id));
    return server.probe.gate(worker::crash_tag(params));
}

void register_pch_store(ServedProjectFixture& fixture, TempDir& tmp) {
    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace(
        {.name = "pch", .extension = ".pch", .aux_extension = ".pch.idx", .max_bytes = 1ull << 30});
    fixture.served->project.store.emplace(std::move(*store));
}

/// Close the document as didClose does: it leaves the master's table, its
/// generation moves on, and the project releases it.
void close_document(ServedProjectFixture& fixture, Fid path_id) {
    auto session = fixture.server.find_session(path_id);
    fixture.server.documents.erase(path_id);
    session->generation += 1;
    session->closed = true;
    fixture.served->release(path_id);
}

/// The hover at `position` of the ticket's buffer, asked of the worker
/// holding the document's AST as the hover request asks it.
async::task<std::optional<feature::HoverInfo>, ipc::Error>
    ask_hover(ServedProject& served,
              const Ticket& ticket,
              protocol::Position position,
              async::cancellation_token token = {}) {
    auto encoding = served.server.position_encoding;
    co_return co_await served.ast_request(
        ticket,
        feature::RequestKind::Hover,
        "Hover",
        worker::HoverParams{
            .offset = ticket.session->positions().offset_clamped(position, encoding),
            .settings = served.project.settings,
        },
        std::move(token));
}

/// A completion at `position` of the ticket's buffer, built on a stateless
/// worker as the completion request asks for it.
async::task<worker::CompletionResult, ipc::Error> ask_completion(ServedProject& served,
                                                                 const Ticket& ticket,
                                                                 protocol::Position position) {
    auto encoding = served.server.position_encoding;
    co_return co_await served.stateless_request(
        ticket,
        feature::RequestKind::Completion,
        "Completion",
        ticket.session->positions().offset_clamped(position, encoding),
        worker::CompletionParams{.encoding = encoding},
        {});
}

ZEST_SUITE(CompileGuards) {

ZEST_CASE(EditedTouchesEntry) {
    ServedProjectFixture fixture;
    auto& served = *fixture.served;
    auto session = fixture.open("/proj/a.cpp", "int x;\n");
    auto pid = session->path_id;
    auto& entry = fixture.entry(pid);
    entry.current = true;
    auto epoch = entry.epoch;

    served.edited(*session);

    ZASSERT(!served.current(pid));
    ZASSERT(entry.epoch == epoch + 1);
}

ZEST_CASE(InvalidateKeepsResult) {
    ServedProjectFixture fixture;
    auto& served = *fixture.served;
    auto session = fixture.open("/proj/a.cpp", "int x;\n");
    auto pid = session->path_id;
    fixture.set_pch_keys(pid, "key", std::nullopt);
    fixture.entry(pid).current = true;

    served.invalidate(pid);

    // Only the currency flag is revoked; the products stay for the
    // bounded-staleness consumers (preamble links, hover gap).
    ZASSERT(!served.current(pid));
    auto compiled = served.latest(pid);
    ZASSERT(compiled != nullptr);
    ZASSERT(compiled->pch_key);
}

ZEST_CASE(ReleaseErasesEntry) {
    ServedProjectFixture fixture;
    auto& served = *fixture.served;
    auto session = fixture.open("/proj/a.cpp", "int x;\n");
    auto pid = session->path_id;
    fixture.set_pch_keys(pid, "key", std::nullopt);

    served.release(pid);

    ZASSERT(served.latest(pid) == nullptr);
    ZASSERT(!served.current(pid));
}

ZEST_CASE(ClosedSessionSkipsCompile) {
    // A caller still holding a closed session asks for its AST: the join
    // ends at once instead of driving rounds that find no session.
    ServedProjectFixture fixture;
    auto& served = *fixture.served;
    auto session = fixture.open("/proj/a.cpp", "int x;\n");
    close_document(fixture, session->path_id);

    bool compiled = true;
    bool done = false;
    auto body = [&]() -> async::task<> {
        compiled = co_await served.ensure_compiled(session);
        co_await served.shutdown();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);
    ZEXPECT(!compiled);
}

ZEST_CASE(ChoiceChangedResets) {
    ServedProjectFixture fixture;
    auto& served = *fixture.served;
    auto session = fixture.open("/proj/h.h", "int x;\n");
    auto pid = session->path_id;
    auto& entry = fixture.entry(pid);
    auto compiled = std::make_shared<DocumentCompile>();
    compiled->pch_key = "key";
    compiled->base_pch_key = "base";
    compiled->output = CompileOutput{.version = 3};
    entry.latest = std::move(compiled);
    entry.deps.emplace();
    entry.current = true;
    auto generation = session->generation;

    served.choice_changed(*session);

    // The new context is a different compilation identity: the state
    // earned under the old one is dropped, but the published output stays
    // until the next compile overwrites it.
    ZASSERT(session->generation == generation + 1);
    ZASSERT(!served.current(pid));
    ZASSERT(!fixture.entry(pid).deps.has_value());
    auto after = served.latest(pid);
    ZASSERT(!after->pch_key.has_value());
    ZASSERT(!after->base_pch_key.has_value());
    ZASSERT(after->output);
}

ZEST_CASE(CrashAfterMoveIgnored) {
    // A request of the project the document left crashes after the move: the
    // crash was under that project's command, and the document keeps no
    // trace of it — no record, no note over what it shows.
    ServedProjectFixture fixture;
    auto& served = *fixture.served;
    auto other = std::make_shared<ServedProject>(fixture.server,
                                                 CanonicalPath(Spelling::absolute("/other")));
    fixture.server.projects.push_back(other);
    auto session = fixture.open("/proj/a.cpp", "int x;\n");
    served.release(session->path_id);
    other->adopt(session);
    fixture.server.documents[session->path_id].owner = other.get();

    served.record_crash(session,
                        feature::RequestKind::Completion,
                        "d1",
                        "killed by signal 11 (SIGSEGV)");
    ZEXPECT(session->crash_record(feature::RequestKind::Completion) == nullptr);
    ZEXPECT(session->crash_notes().empty());
    ZEXPECT(other->latest(session->path_id) == nullptr);

    // Nor does a compile's crash make its clang-tidy pass a suspect: the
    // other project's compiles keep running it.
    ZEXPECT(!fixture.attribute_crash(session, true, ipc::Error{"killed by signal 11 (SIGSEGV)"}));
    ZEXPECT(!session->suspects.tidy);
    ZEXPECT(session->crash_record(feature::RequestKind::Compile) == nullptr);
}

ZEST_CASE(CrashPublishesNote) {
    // A recorded compile crash publishes its note at once — the progress
    // end that follows publishes nothing more — and takes the old diagnostics down:
    // no AST stands behind them anymore.
    ServedProjectFixture fixture;
    auto& served = *fixture.served;
    auto session = fixture.open("/proj/poison.cpp", "int x;\n");
    fixture.set_output(session->path_id,
                       CompileOutput{
                           .version = 1,
                           .diagnostics = {.own = {feature::file_warning("old")}},
                       });

    auto before = served.latest(session->path_id);
    served.record_crash(session,
                        feature::RequestKind::Compile,
                        "d1",
                        "killed by signal 11 (SIGSEGV)");
    auto compiled = served.latest(session->path_id);
    ZEXPECT(compiled != before);
    ZASSERT((compiled && compiled->output.has_value()));
    ZEXPECT(compiled->output->diagnostics.own.empty());

    auto notes = session->crash_notes();
    ZASSERT(notes.size() == 1u);
    auto message = feature::crash_note(notes[0]).message;
    ZEXPECT(message.contains("while compiling this file (killed by signal 11 (SIGSEGV))"));
    ZEXPECT(message.contains("save it"));

    bool done = false;
    auto body = [&]() -> async::task<> {
        ZASSERT(!co_await served.ensure_compiled(session));
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);
    ZEXPECT(served.latest(session->path_id) == compiled);
}

ZEST_CASE(ShutdownUnblocksWaiters) {
    // A waiter parked on an in-flight round must resolve at shutdown:
    // ServedProject::stop_compiles interrupts the parse (the send itself
    // carries no token by design), so the round finishes promptly and the
    // graph's shutdown finds nothing to wait out.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("slow.cpp", "");
    auto src = tmp.path("slow.cpp");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    std::string text;
    text.reserve(1 << 22);
    for(int i = 0; i < 200'000; i += 1) {
        text += std::format("int v{};\n", i);
    }
    auto session = fixture.open(src, std::move(text));

    bool waiter_done = false;
    bool waiter_ok = true;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        async::task_group<> group;
        auto waiter = [&]() -> async::task<> {
            waiter_ok = co_await served.ensure_compiled(session);
            waiter_done = true;
        };
        auto gate = gate_compile(fixture, session->path_id);
        group.spawn(waiter());
        co_await parked(fixture.server.probe, gate);
        ZASSERT(served.compiling(session->path_id));
        ZASSERT(!waiter_done);

        co_await served.stop_compiles();
        fixture.server.probe.release(gate);
        co_await group.join();

        ZEXPECT(waiter_done);
        ZEXPECT(!waiter_ok);
        ZEXPECT(!served.compiling(session->path_id));

        // Not served.shutdown(): it stops the compiles again, and a task
        // group is joined once.
        co_await served.sched.graph.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
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
    // StatefulWorker.IpcCancelInterruptsCompile.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("edited_only.cpp", "");
    auto src = tmp.path("edited_only.cpp");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto session = fixture.open(src, "int v0;\n");

    bool waiter_done = false;
    bool waiter_ok = false;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        async::task_group<> group;
        auto waiter = [&]() -> async::task<> {
            waiter_ok = co_await served.ensure_compiled(session);
            waiter_done = true;
        };
        auto gate = gate_compile(fixture, session->path_id);
        group.spawn(waiter());
        co_await parked(fixture.server.probe, gate);
        ZASSERT(served.compiling(session->path_id));

        // What didChange does: fold the edit in, then tell the project.
        session->set_text("int fixed;\n");
        session->generation += 1;
        served.edited(*session);
        fixture.server.probe.release(gate);
        co_await group.join();

        ZASSERT(waiter_done);
        ZEXPECT(!waiter_ok);

        // The next request (the editor re-queries after an edit) compiles
        // the fresh content.
        bool second_ok = co_await served.ensure_compiled(session);
        ZEXPECT(second_ok);
        ZEXPECT(served.current(session->path_id));

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(SupersededCompileCancelled) {
    // An edit mid-compile supersedes the in-flight round: the first waiter
    // abandons (its buffer is gone), the edit interrupts the worker's
    // parse, and the second waiter's respawned round compiles the new
    // content. This pins the edit path's liveness; the interruption itself
    // is pinned content-wise by StatefulWorker.IpcCancelInterruptsCompile.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("edited.cpp", "");
    auto src = tmp.path("edited.cpp");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto session = fixture.open(src, "int v0;\n");

    bool first_done = false;
    bool second_ok = false;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        async::task_group<> group;
        auto first = [&]() -> async::task<> {
            [[maybe_unused]] bool ok = co_await served.ensure_compiled(session);
            first_done = true;
        };
        auto gate = gate_compile(fixture, session->path_id);
        group.spawn(first());
        co_await parked(fixture.server.probe, gate);
        ZASSERT(served.compiling(session->path_id));

        // The edit arrives while the slow compile is in flight.
        session->set_text("int fixed;\n");
        session->generation += 1;
        served.edited(*session);

        auto second = [&]() -> async::task<> {
            second_ok = co_await served.ensure_compiled(session);
        };
        group.spawn(second());
        fixture.server.probe.release(gate);
        co_await group.join();

        ZEXPECT(first_done);
        ZEXPECT(second_ok);
        ZEXPECT(served.current(session->path_id));

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
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

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto& project = served.project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"), {}},
                  {tmp.root, src,                {}},
    }));
    scan_all(project.cdb, project.toolchain, project.dep_graph);
    project.dep_graph.build_reverse_map();
    register_pch_store(fixture, tmp);
    project.store->register_namespace(
        {.name = "pcm", .extension = ".pcm", .max_bytes = 1ull << 30});

    auto session = fixture.open(src, "import m;\nint main() { return mv(); }\n");

    bool ok = false;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        ok = co_await served.ensure_compiled(session);

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    ZEXPECT(ok);
    ZEXPECT(served.current(session->path_id));
    auto mod_ids = project.dep_graph.lookup_module("m");
    ZASSERT(!mod_ids.empty());
    auto build = project.pcm_builds.find(mod_ids[0]);
    ZASSERT(build != project.pcm_builds.end());
    ZEXPECT(!build->second.path.empty());
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

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto& project = served.project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("base.cppm"),  {}},
                  {tmp.root, tmp.path("left.cppm"),  {}},
                  {tmp.root, tmp.path("right.cppm"), {}},
                  {tmp.root, tmp.path("top.cppm"),   {}},
                  {tmp.root, src,                    {}},
    }));
    scan_all(project.cdb, project.toolchain, project.dep_graph);
    project.dep_graph.build_reverse_map();
    register_pch_store(fixture, tmp);
    project.store->register_namespace(
        {.name = "pcm", .extension = ".pcm", .max_bytes = 1ull << 30});

    auto session = fixture.open(src, "import top;\nint main() { return t(); }\n");

    bool ok = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        ok = co_await served.ensure_compiled(session);

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZASSERT(ok);

    auto top = project.dep_graph.lookup_module("top");
    ZASSERT(!top.empty());
    auto entry = project.pcm_builds.find(top[0]);
    ZASSERT(entry != project.pcm_builds.end());
    auto base = fixture.intern(tmp.path("base.cppm"));
    auto& deps = entry->second.deps;
    ZEXPECT(ranges::count_if(deps, [&](const DepState& dep) { return dep.path_id == base; }) == 1);
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

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto& project = served.project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"),    {}},
                  {tmp.root, tmp.path("main.cpp"),  {}},
                  {tmp.root, tmp.path("plain.cpp"), {}},
    }));
    scan_all(project.cdb, project.toolchain, project.dep_graph);
    project.dep_graph.build_reverse_map();
    register_pch_store(fixture, tmp);
    project.store->register_namespace(
        {.name = "pcm", .extension = ".pcm", .max_bytes = 1ull << 30});

    auto plain = fixture.open(tmp.path("plain.cpp"), "int plain() { return 0; }\n");
    auto main = fixture.open(tmp.path("main.cpp"), "import m;\nint main() { return mv(); }\n");
    auto edit = [&](const std::shared_ptr<Session>& session, std::string text) {
        session->set_text(std::move(text));
        session->generation += 1;
        served.edited(*session);
    };

    std::uint64_t plain_scans = 0;
    std::uint64_t first_scans = 0;
    std::uint64_t body_scans = 0;
    std::uint64_t import_scans = 0;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        auto& pcm = served.sched.pcm;
        ZASSERT(co_await served.ensure_compiled(plain));
        plain_scans = pcm.import_scans;
        ZASSERT(co_await served.ensure_compiled(main));
        first_scans = pcm.import_scans;

        edit(main, "import m;\nint main() { return mv() + 1; }\n");
        ZASSERT(co_await served.ensure_compiled(main));
        body_scans = pcm.import_scans;

        edit(main, "import m;\n#define TWO 2\nint main() { return mv() + TWO; }\n");
        ZASSERT(co_await served.ensure_compiled(main));
        import_scans = pcm.import_scans;

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
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

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto session = fixture.open(src, "import m;\nint main() { return 0; }\n");

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        [[maybe_unused]] bool ok = co_await served.ensure_compiled(session);

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    // The sentinel edge survives the commit: a first provider's update
    // must reach this document's node.
    auto dirtied = served.sched.graph.update(PCMBuilder::unresolved_node("m"));
    ZEXPECT(ranges::find(dirtied, NodeId{Family::AST, session->path_id.raw}) != dirtied.end());

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

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto& project = served.project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, src, {"-include", tmp.path("deps.h")}}
    }));
    scan_all(project.cdb, project.toolchain, project.dep_graph);
    project.dep_graph.build_reverse_map();
    auto session = fixture.open(src, "int main() { return 0; }\n");

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        [[maybe_unused]] bool ok = co_await served.ensure_compiled(session);

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    auto dirtied = served.sched.graph.update(PCMBuilder::unresolved_node("m"));
    ZEXPECT(ranges::find(dirtied, NodeId{Family::AST, session->path_id.raw}) != dirtied.end());

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

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto& project = served.project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    scan_all(project.cdb, project.toolchain, project.dep_graph);
    project.dep_graph.build_reverse_map();
    auto session = fixture.open(tmp.path("part.inc"), "int part();\n");

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        [[maybe_unused]] bool ok = co_await served.ensure_compiled(session);

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    auto* context = project.hosting.contexts.find(session->path_id);
    ZASSERT((context != nullptr && context->result.synthesized != nullptr));
    auto dirtied = served.sched.graph.update(PCMBuilder::unresolved_node("m"));
    ZEXPECT(ranges::find(dirtied, NodeId{Family::AST, session->path_id.raw}) != dirtied.end());

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(SuffixImportRecorded) {
    // An import past the header's include, inside the braces around it,
    // is in the suffix the compile appends: the precise scan reads it too.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("part.inc", "int part();\n");
    tmp.touch("main.cpp",
              "extern \"C++\" {\n#include \"part.inc\"\nimport m;\n}\nint main() { return 0; }\n");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto& project = served.project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    scan_all(project.cdb, project.toolchain, project.dep_graph);
    project.dep_graph.build_reverse_map();
    auto session = fixture.open(tmp.path("part.inc"), "int part();\n");

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        [[maybe_unused]] bool ok = co_await served.ensure_compiled(session);

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    auto* context = project.hosting.contexts.find(session->path_id);
    ZASSERT((context != nullptr && context->result.synthesized != nullptr));
    auto dirtied = served.sched.graph.update(PCMBuilder::unresolved_node("m"));
    ZEXPECT(ranges::find(dirtied, NodeId{Family::AST, session->path_id.raw}) != dirtied.end());

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

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto& project = served.project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, src, {"-include", "force.h"}}
    }));
    scan_all(project.cdb, project.toolchain, project.dep_graph);
    project.dep_graph.build_reverse_map();
    auto unit = fixture.open(src, "#include \"header.h\"\nint main() { return header(); }\n");
    auto header = fixture.open(tmp.path("header.h"), "inline int header() { return FORCED; }\n");

    bool unit_ok = false;
    bool header_ok = false;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        unit_ok = co_await served.ensure_compiled(unit);
        header_ok = co_await served.ensure_compiled(header);

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    ZEXPECT(unit_ok);
    ZEXPECT(header_ok);
    ZEXPECT(served.latest(header->path_id)->host == unit->path_id);
    ZEXPECT(served.sched.pcm.import_scans == 0u);
}

};  // ZEST_SUITE(CompileGuards)

ZEST_SUITE(AnswerGuards) {

ZEST_CASE(CrashedCompileStopsBuilds) {
    // A stopped compile stops the stateless builds of the same content
    // too — completion parses what the compile crashed on — and they answer
    // empty: the crash note says why, an error would only be a popup.
    ServedProjectFixture fixture;
    auto& served = *fixture.served;
    auto session = fixture.open("/proj/poison.cpp", "int x;\n");
    session->crashed(feature::RequestKind::Compile, "d1", "cause", Session::Clock::now());

    bool done = false;
    auto body = [&]() -> async::task<> {
        // With no worker at all, only the stop can answer without an error.
        auto result = co_await ask_completion(served, Ticket::take(session), {});
        ZASSERT(result);
        ZEXPECT(result.value().list.data == "null");
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);
}

ZEST_CASE(CrashedFormatStopsFormat) {
    ServedProjectFixture fixture;
    auto& served = *fixture.served;
    auto session = fixture.open("/proj/poison.cpp", "int x;\n");
    session->crashed(feature::RequestKind::Format, "d1", "cause", Session::Clock::now());

    bool done = false;
    auto body = [&]() -> async::task<> {
        auto result = co_await served.format(Ticket::take(session), std::nullopt, {});
        ZASSERT(result);
        ZEXPECT(!result.value().has_value());
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);
}

ZEST_CASE(EpochGuardsPCHWrite) {
    ServedProjectFixture fixture;
    auto& served = *fixture.served;
    auto session = fixture.open("/proj/a.cpp", "int x;");
    auto pid = session->path_id;
    // No preamble directives: a current request would take the pch_key
    // reset branch; an invalidated continuation must not touch it.
    fixture.set_pch_keys(pid, "key", std::nullopt);

    auto gen = session->generation;
    auto epoch = fixture.entry(pid).epoch;
    // A Lost-type invalidation (disk/CDB change behind the in-flight
    // request) arrives after takeoff: the document's epoch bumps,
    // generation stays.
    served.invalidate(pid);

    std::string directory = "/proj";
    std::vector<std::string> arguments = {"clang++", "-fsyntax-only", "/proj/a.cpp"};
    bool wrote = true;
    auto body = [&]() -> async::task<> {
        wrote = co_await fixture.ensure_pch(session, gen, epoch, directory, arguments);
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();

    ZEXPECT(!wrote);
    // The stale continuation left the adopted PCH reference untouched.
    auto compiled = served.latest(pid);
    ZASSERT((compiled && compiled->pch_key.has_value()));
    ZEXPECT(*compiled->pch_key == std::string("key"));
}

ZEST_CASE(PCHCrashStopsDocument) {
    // A PCH build that kills its stateless worker stops the document: the
    // preamble is its content too. The worker names the build, so the
    // death is attributed to it at once and the build is not resent.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    register_pch_store(fixture, tmp);
    auto session = fixture.open(src, "#pragma clang __debug crash\n");

    std::string directory = tmp.path(".");
    auto arguments = make_args(src);

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(fixture.server.pool.start(opts));

        bool built = co_await fixture.ensure_pch(session,
                                                 session->generation,
                                                 fixture.entry(session->path_id).epoch,
                                                 directory,
                                                 arguments);
        ZEXPECT(!built);
        ZEXPECT(served.compile_stopped(*session));
        auto notes = session->crash_notes();
        ZASSERT(notes.size() == 1u);
        ZEXPECT(notes[0].kind == feature::RequestKind::PCH);
        ZEXPECT(notes[0].count == 1u);

        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(PCHCrashStopsBuild) {
    // A PCH crash inside a completion build's dependency prep stops the
    // document after the entry gate: the build stops instead of
    // dispatching the same content to one more worker.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    register_pch_store(fixture, tmp);
    auto session = fixture.open(src, "#pragma clang __debug crash\nint value;\n");

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(fixture.server.pool.start(opts));

        // Past the preamble: a completion before it parses none of it.
        auto result =
            co_await ask_completion(served, Ticket::take(session), {.line = 1, .character = 0});
        ZASSERT(result);
        ZEXPECT(result.value().list.data == "null");
        auto notes = session->crash_notes();
        ZASSERT(notes.size() == 1u);
        ZEXPECT(notes[0].count == 1u);

        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(QueuedRequestStops) {
    // Two completions of the same content: the first crashes its worker
    // while the second waits in the queue for that only worker. The second
    // is checked where it would reach the worker, after its wait, and the
    // crash of its kind on its content stops it there: no second death.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    register_pch_store(fixture, tmp);
    auto session = fixture.open(src, "int value;\n#pragma clang __debug crash\nint other;\n");

    int deaths = 0;
    fixture.server.pool.on_crash = [&](const WorkerCrashInfo&) {
        deaths += 1;
    };

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        opts.max_stateless = 1;
        ZASSERT(fixture.server.pool.start(opts));

        auto complete = [&]() -> async::task<> {
            auto result =
                co_await ask_completion(served, Ticket::take(session), {.line = 2, .character = 0});
            ZASSERT(result);
            ZEXPECT(result.value().list.data == "null");
        };
        co_await async::when_all(complete(), complete());
        ZEXPECT(deaths == 1);
        auto notes = session->crash_notes();
        ZASSERT(notes.size() == 1u);
        ZEXPECT(notes[0].kind == feature::RequestKind::Completion);
        ZEXPECT(notes[0].count == 1u);

        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
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

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto session = fixture.open(src, "int v0;\n");

    bool cancelled_returned = false;
    bool other_answered = false;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        async::cancellation_source source;
        async::task_group<> group;
        auto cancelled_waiter = [&]() -> async::task<> {
            auto hover = [&]() -> async::task<std::optional<feature::HoverInfo>, ipc::Error> {
                co_return co_await ask_hover(served,
                                             Ticket::take(session),
                                             protocol::Position{0, 4},
                                             source.token());
            };
            auto r = co_await async::with_token(hover(), source.token());
            cancelled_returned = r.is_cancelled();
        };
        auto other_waiter = [&]() -> async::task<> {
            auto result =
                co_await ask_hover(served, Ticket::take(session), protocol::Position{0, 4});
            other_answered = result.has_value();
        };
        auto gate = gate_compile(fixture, session->path_id);
        group.spawn(cancelled_waiter());
        group.spawn(other_waiter());
        co_await parked(fixture.server.probe, gate);
        ZASSERT(served.compiling(session->path_id));
        source.cancel();
        fixture.server.probe.release(gate);
        co_await group.join();

        ZEXPECT(cancelled_returned);
        ZEXPECT(other_answered);
        ZEXPECT(served.current(session->path_id));
        ZEXPECT(!served.compiling(session->path_id));

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(StaleReplyBecomesContentModified) {
    // A reply for a buffer the client edited away never leaves as a value:
    // it becomes ContentModified even when the worker
    // answered after the compile had finished — the edit arrived while the
    // query itself was in flight.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("stale.cpp", "");
    auto src = tmp.path("stale.cpp");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto session = fixture.open(src, "int v0;\n");

    bool stale = false;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        // Warm the AST: the next query's only suspension is its worker send.
        auto warm = co_await ask_hover(served, Ticket::take(session), protocol::Position{0, 4});
        ZASSERT(warm);

        auto ticket = Ticket::take(session);
        async::task_group<> group;
        auto tokens = [&]() -> async::task<> {
            auto result = co_await served.ast_request(ticket,
                                                      feature::RequestKind::SemanticTokens,
                                                      "SemanticTokens",
                                                      worker::SemanticTokensParams{
                                                          .settings = served.project.settings,
                                                      },
                                                      {});
            stale = !result.has_value() && result.error().code == content_modified().code;
        };
        group.spawn(tokens());
        // spawn runs the query inline to that suspension, so the pool
        // already reports it in flight: the bump arrives provably between
        // dispatch and reply.
        ZASSERT(fixture.server.pool.stateful_busy());
        session->generation += 1;
        co_await group.join();
        ZEXPECT(stale);

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(HarmlessKindKeepsStop) {
    // A crash of one query kind stops only that kind: a hover on a document
    // whose semantic tokens crashed is ordinary work — it is answered, and
    // the tokens record stays as it was.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto session = fixture.open(src, "int x;\n");
    auto tokens = feature::RequestKind::SemanticTokens;
    session->crashed(tokens, "w-1", "cause", Session::Clock::now());

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        auto result = co_await ask_hover(served, Ticket::take(session), protocol::Position{0, 4});
        ZEXPECT(result);
        ZEXPECT(session->crash_stops(tokens, Session::Clock::now()));

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(PoisonPreambleShared) {
    // One document's records cannot contain a poison preamble alone: the
    // PCH is shared, so every document with the same preamble would
    // re-trigger the build and burn a worker of its own. After one crashed
    // build the key is refused before any dispatch, and every other
    // document books the same death instead of a fresh one.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    TempDir tmp;
    ServedProjectFixture fixture(tmp.root);
    register_pch_store(fixture, tmp);

    auto open_poisoned = [&](llvm::StringRef name) {
        tmp.touch(name, "");
        return fixture.open(tmp.path(name), "#pragma clang __debug crash\n");
    };
    auto first = open_poisoned("a.cpp");
    auto second = open_poisoned("b.cpp");
    auto third = open_poisoned("c.cpp");

    std::string directory = tmp.path(".");
    auto arguments = make_args(tmp.path("a.cpp"));

    int deaths = 0;
    fixture.server.pool.on_crash = [&](const WorkerCrashInfo&) {
        deaths += 1;
    };

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(fixture.server.pool.start(opts));

        auto build = [&](const std::shared_ptr<Session>& session) {
            return fixture.ensure_pch(session,
                                      session->generation,
                                      fixture.entry(session->path_id).epoch,
                                      directory,
                                      arguments);
        };
        auto crashed = [&](const std::shared_ptr<Session>& session) {
            return session->crash_record(feature::RequestKind::PCH) != nullptr;
        };
        ZASSERT(!co_await build(first));
        ZASSERT(crashed(first));
        ZEXPECT(deaths == 1);

        // Refused without touching a worker, each document stopped by the
        // same death.
        ZASSERT(!co_await build(second));
        ZEXPECT(crashed(second));
        ZASSERT(!co_await build(third));
        ZEXPECT(crashed(third));
        ZEXPECT(deaths == 1);

        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(EpochGuardsPCHClear) {
    // A successful build whose epoch moved mid-flight must not clear the
    // document's PCH record: the crash belongs to content the request no
    // longer describes, and clearing it would let a poison preamble dodge
    // its stop behind an old round's commit.
    TempDir tmp;
    tmp.touch("a.cpp", "");
    auto src = tmp.path("a.cpp");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    register_pch_store(fixture, tmp);
    auto session = fixture.open(src, "#define X 1\nint x;\n");
    // One prior crash on the PCH record.
    auto crashed = [&] {
        return session->crash_record(feature::RequestKind::PCH) != nullptr;
    };
    session->crashed(feature::RequestKind::PCH, "w-1", "cause", Session::Clock::now());

    std::string directory = tmp.path(".");
    auto arguments = make_args(src);

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(fixture.server.pool.start(opts));

        auto gen = session->generation;
        auto epoch = fixture.entry(session->path_id).epoch;
        bool built = true;
        auto launch = [&]() -> async::task<> {
            built = co_await fixture.ensure_pch(session, gen, epoch, directory, arguments);
        };
        // Runs after the launch suspended on its dispatched build: a
        // Lost-type invalidation arrives behind the in-flight request.
        auto invalidate = [&]() -> async::task<> {
            served.invalidate(session->path_id);
            co_return;
        };
        co_await async::when_all(launch(), invalidate());

        // The build finished (the shared artifact is cached) and the stale
        // request compiles against it, but it adopted nothing and cleared
        // nothing.
        ZEXPECT(built);
        auto compiled = served.latest(session->path_id);
        ZEXPECT((!compiled || !compiled->pch_key.has_value()));
        ZEXPECT(crashed());

        // A current request adopts the cached pair and only then clears
        // this document's record.
        built = co_await fixture.ensure_pch(session,
                                            session->generation,
                                            fixture.entry(session->path_id).epoch,
                                            directory,
                                            arguments);
        ZEXPECT(built);
        compiled = served.latest(session->path_id);
        ZEXPECT((compiled && compiled->pch_key.has_value()));
        ZEXPECT(!crashed());

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
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

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    register_pch_store(fixture, tmp);
    auto session = fixture.open(src, "#include \"dep.h\"\nint x;\n");

    std::string directory = tmp.path(".");
    auto arguments = make_args(src, "-std=c++20");
    arguments.insert(arguments.begin() + 1, "-fmodule-file=broken=" + tmp.path("broken.pcm"));

    auto build = [&] {
        return fixture.ensure_pch(session,
                                  session->generation,
                                  fixture.entry(session->path_id).epoch,
                                  directory,
                                  arguments);
    };

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(fixture.server.pool.start(opts));

        ZASSERT(co_await build());
        auto adopted = served.latest(session->path_id);
        ZASSERT((adopted && adopted->pch_key.has_value()));
        auto adopted_key = *adopted->pch_key;

        // The header the pair depends on changes into one importing a
        // module that cannot load: the pair is deps-stale and its rebuild
        // fails, as no PCH is written past a failed module load.
        tmp.touch("dep.h", "import broken;\n");

        bool first_built = true;
        bool second_built = true;
        auto acquire_first = [&]() -> async::task<> {
            first_built = co_await build();
        };
        auto acquire_second = [&]() -> async::task<> {
            second_built = co_await build();
        };
        co_await async::when_all(acquire_first(), acquire_second());

        ZEXPECT(!first_built);
        ZEXPECT(!second_built);
        // The latest compile keeps the first adoption — the failed rebuild
        // wrote nothing over it, and the stale pair heals on a later
        // successful acquisition.
        auto compiled = served.latest(session->path_id);
        ZEXPECT((compiled && compiled->pch_key == adopted_key));

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);
}

ZEST_CASE(EvictedDocumentRecompiles) {
    // The worker evicts a document behind the master's back (no eviction
    // notice is routed here): the query hears document_unloaded, compiles it
    // there again and answers, never null.
    TempDir tmp;
    tmp.touch("a.cpp", "");
    tmp.touch("b.cpp", "");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto a = fixture.open(tmp.path("a.cpp"), "int alpha = 1;\n");
    auto b = fixture.open(tmp.path("b.cpp"), "int beta = 2;\n");

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        opts.max_documents = 1;
        ZASSERT(fixture.server.pool.start(opts));

        ZASSERT(co_await served.ensure_compiled(a));
        ZASSERT(co_await served.ensure_compiled(b));
        auto result = co_await ask_hover(served, Ticket::take(a), protocol::Position{0, 5});
        ZASSERT(result);
        ZEXPECT(result.value().has_value());

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);
}

ZEST_CASE(EvictedAgainRecompiles) {
    // The worker loses the document again between the recompile's reply and
    // the query — another document's request arriving in that window: the
    // query compiles it there again for every eviction it meets.
    TempDir tmp;
    tmp.touch("a.cpp", "");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto a = fixture.open(tmp.path("a.cpp"), "int alpha = 1;\n");
    auto path = std::string(served.project.file_table.resolve(a->path_id));
    auto evict = [&] {
        fixture.server.pool.notify_stateful(a->path_id.raw, worker::EvictParams{path});
    };
    auto& probe = fixture.server.probe;
    fixture.server.pool.probe = &probe;
    auto connection = probe.on_held.connect([&](std::uint64_t id) {
        evict();
        probe.release(id);
    });

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        ZASSERT(co_await served.ensure_compiled(a));
        evict();
        probe.hold(BuildKind::Compile, path);
        auto result = co_await ask_hover(served, Ticket::take(a), protocol::Position{0, 5});
        ZASSERT(result);
        ZEXPECT(result.value().has_value());
        ZEXPECT(probe.builds[path][std::to_underlying(BuildKind::Compile)] == 3u);

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);
}

ZEST_CASE(AnswerClearsQueryRecord) {
    // Only an answer of the kind clears its record.
    TempDir tmp;
    tmp.touch("a.cpp", "");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto a = fixture.open(tmp.path("a.cpp"), "int alpha = 1;\n");
    auto hover = feature::RequestKind::Hover;
    a->crashed(hover, "d1", "cause", Session::Clock::now());
    a->saved();

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        ZASSERT(co_await served.ensure_compiled(a));
        auto result = co_await ask_hover(served, Ticket::take(a), protocol::Position{0, 5});
        ZASSERT(result);
        ZEXPECT(result.value().has_value());
        ZEXPECT(!a->crash_record(hover));

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);
}

ZEST_CASE(CloseDuringQueryEnds) {
    // The document closes while its query is in flight, and the worker,
    // already told to evict it, answers document_unloaded: the query ends
    // ContentModified instead of compiling the closed session again.
    TempDir tmp;
    tmp.touch("a.cpp", "");

    ServedProjectFixture fixture(tmp.root);
    auto& served = *fixture.served;
    auto a = fixture.open(tmp.path("a.cpp"), "int alpha = 1;\n");
    auto path_id = a->path_id;

    bool modified = false;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 0;
        opts.stateful_count = 1;
        ZASSERT(fixture.server.pool.start(opts));

        ZASSERT(co_await served.ensure_compiled(a));
        fixture.server.pool.notify_stateful(
            path_id.raw,
            worker::EvictParams{std::string(served.project.file_table.resolve(path_id))});

        auto ticket = Ticket::take(a);
        async::task_group<> group;
        auto hover = [&]() -> async::task<> {
            auto result = co_await ask_hover(served, ticket, protocol::Position{0, 5});
            modified = !result.has_value() && result.error().code == content_modified().code;
        };
        group.spawn(hover());
        ZASSERT(fixture.server.pool.stateful_busy());
        close_document(fixture, path_id);
        co_await group.join();
        ZEXPECT(modified);

        co_await served.shutdown();
        co_await fixture.server.pool.stop();
        done = true;
    };
    auto task = body();
    fixture.loop.schedule(task);
    fixture.loop.run();
    ZEXPECT(done);
}

};  // ZEST_SUITE(AnswerGuards)

}  // namespace

}  // namespace clice::testing
