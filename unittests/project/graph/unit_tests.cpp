module clice;

import :compile.compilation;
import :project.graph.precompiled;
import :project.graph.task_graph;
import :project.graph.unit;
import :support.anomaly;
import :unittests.server.worker_test_helpers;
import :unittests.utils;
import :unittests.utils.cdb_helper;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :vfs.cache_store;
import :vfs.path;
import :worker.pool;

namespace clice::testing {
namespace {

ZEST_SUITE(UnitLint) {

ZEST_CASE(ModuleLintScanParity) {
    // Only a scan under the lint pass's extra args discovers the import
    // they gate, and edges it so the PCM exists when the worker's parse
    // (which sees the extras) consumes it. Only n.cppm runs, so the
    // import's PCM cannot arrive any other way.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\nexport int mv() { return 1; }\n");
    tmp.touch("n.cppm",
              "export module n;\n"
              "#ifdef USE_M\n"
              "import m;\n"
              "export double half(int a, int b) { return a / b; }\n"
              "#endif\n");

    IndexerFixture f;
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"), {}},
                  {tmp.root, tmp.path("n.cppm"), {}},
    }));
    scan_all(f.project.cdb, f.project.toolchain, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace({.name = "pcm", .extension = ".pcm", .max_bytes = 1ull << 30});
    f.project.store.emplace(std::move(*store));

    UnitRequest request;
    request.tidy = tidy::TidyParams{
        .checks = "-*,bugprone-integer-division",
        .extra_args = {"-DUSE_M"},
    };

    auto n_id = f.project.file_table.intern(Spelling::absolute(tmp.path("n.cppm")));
    std::optional<UnitResult> result;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        result = co_await f.project.compile_unit(n_id, std::move(request));

        co_await f.project.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    ZASSERT(result);
    ZEXPECT(result->status == UnitResult::Status::Completed);
    // The finding inside the gated region proves the parse saw the
    // extras and consumed the edge-built PCM.
    ZASSERT(!result->tidy_diagnostics.empty());
    ZEXPECT(result->tidy_diagnostics[0].check == "bugprone-integer-division");
}

};  // ZEST_SUITE(UnitLint)

ZEST_SUITE(UnitIndex) {

ZEST_CASE(OutdatedRerunsInFull) {
    // Two headers with the same bytes share one variant identity. Only
    // x1.h stores it, so b.cpp's x2.h section matches a known hash while
    // its own file stores nothing: that result must not commit, and the
    // rerun sends every section.
    TempDir tmp;
    tmp.touch("x1.h", "#pragma once\nint f();\n");
    tmp.touch("x2.h", "#pragma once\nint f();\n");
    tmp.touch("a.cpp", "#include \"x1.h\"\nint a() { return f(); }\n");
    tmp.touch("b.cpp", "#include \"x1.h\"\n#include \"x2.h\"\nint b() { return f(); }\n");

    IndexerFixture f;
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("a.cpp"), {}},
                  {tmp.root, tmp.path("b.cpp"), {}},
    }));
    scan_all(f.project.cdb, f.project.toolchain, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    auto a = index_file(tmp, tmp.path("a.cpp"));
    ZASSERT(f.merge(a.data));
    auto x2_id = f.project.file_table.intern(Spelling::absolute(tmp.path("x2.h")));
    ZASSERT(!f.project.index.shard(x2_id));

    auto b_id = f.project.file_table.intern(Spelling::absolute(tmp.path("b.cpp")));
    std::vector<UnitResult::Status> results;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        for(int run = 0; run < 2; run += 1) {
            UnitRequest request;
            request.index = true;
            results.push_back((co_await f.project.compile_unit(b_id, std::move(request)))->status);
        }

        co_await f.project.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    using enum UnitResult::Status;
    ZEXPECT(results == (std::vector<UnitResult::Status>{Preempted, Completed}));
    ZEXPECT(f.project.index.shard(x2_id) != nullptr);
}

ZEST_CASE(BuildsOnlyImportedPCMs) {
    // Indexing a module unit needs the PCMs of its imports, never its own:
    // the interface and an implementation partition nothing imports build
    // none, the implementation unit builds the interface it imports.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\nexport int mv() { return 1; }\n");
    tmp.touch("detail.cpp", "module m:detail;\nint dv() { return 2; }\n");
    tmp.touch("impl.cpp", "module m;\nint uv() { return mv(); }\n");

    IndexerFixture f;
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"),     {}},
                  {tmp.root, tmp.path("detail.cpp"), {}},
                  {tmp.root, tmp.path("impl.cpp"),   {}},
    }));
    scan_all(f.project.cdb, f.project.toolchain, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace({.name = "pcm", .extension = ".pcm", .max_bytes = 1ull << 30});
    f.project.store.emplace(std::move(*store));

    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    auto built_pcms = [&] {
        return ranges::count_if(f.project.pcms,
                                [](auto& entry) { return !entry.second.path.empty(); });
    };
    std::vector<UnitResult::Status> results;
    std::size_t pcms_before_impl = 0;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        for(auto name: {"m.cppm", "detail.cpp", "impl.cpp"}) {
            if(llvm::StringRef(name) == "impl.cpp") {
                pcms_before_impl = built_pcms();
            }
            UnitRequest request;
            request.index = true;
            results.push_back(
                (co_await f.project.compile_unit(id(name), std::move(request)))->status);
        }

        co_await f.project.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    using enum UnitResult::Status;
    ZEXPECT(results == (std::vector<UnitResult::Status>{Completed, Completed, Completed}));
    ZEXPECT(pcms_before_impl == 0u);
    ZEXPECT(built_pcms() == 1);
    auto build = f.project.pcms.find(id("m.cppm"));
    ZASSERT(build != f.project.pcms.end());
    ZEXPECT(!build->second.path.empty());
}

ZEST_CASE(FailedImportWaitsOnce) {
    // Waiting on the imports again helps only while every one built: one
    // that failed ends the wait at the first pass, its failure told once,
    // while the others built.
    TempDir tmp;
    tmp.touch("good.cppm", "export module good;\nexport int gv() { return 1; }\n");
    tmp.touch("bad.cppm", "export module bad;\nexport int bv() { return undeclared; }\n");

    IndexerFixture f;
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("good.cppm"), {}},
                  {tmp.root, tmp.path("bad.cppm"),  {}},
    }));
    scan_all(f.project.cdb, f.project.toolchain, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace({.name = "pcm", .extension = ".pcm", .max_bytes = 1ull << 30});
    f.project.store.emplace(std::move(*store));

    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    llvm::SmallVector<Fid> modules{id("bad.cppm"), id("good.cppm")};
    llvm::SmallVector<Fid> failed;
    std::optional<Result> waited;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        auto heard = [&](Fid module) {
            failed.push_back(module);
        };
        Work importer{.run = [&](Attempt& attempt) {
            return f.project.wait_modules(&attempt, modules, heard);
        }};
        waited = co_await f.project.graph.join({NodeKind::Unit, id("user.cpp").raw}, importer);

        co_await f.project.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    ZEXPECT(waited == Result::Failed);
    ZEXPECT(failed == llvm::SmallVector<Fid>{id("bad.cppm")});
    ZEXPECT(f.project.built_pcm(id("good.cppm")) != nullptr);
}

ZEST_CASE(EvictedModuleBelowRebuilds) {
    // A module two imports down lost its PCM while every node stayed clean:
    // the unit's wait finds it gone below the module it imports, and both
    // rebuild before the parse reads them.
    TempDir tmp;
    tmp.touch("low.cppm", "export module low;\nexport int lv() { return 1; }\n");
    tmp.touch("mid.cppm", "export module mid;\nimport low;\nexport int mv() { return lv(); }\n");
    tmp.touch("use.cpp", "import mid;\nint u() { return mv(); }\n");

    IndexerFixture f;
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("low.cppm"), {}},
                  {tmp.root, tmp.path("mid.cppm"), {}},
                  {tmp.root, tmp.path("use.cpp"),  {}},
    }));
    scan_all(f.project.cdb, f.project.toolchain, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace({.name = "pcm", .extension = ".pcm", .max_bytes = 1ull << 30});
    f.project.store.emplace(std::move(*store));

    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    std::vector<UnitResult::Status> results;
    std::string evicted;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        UnitRequest first;
        first.index = true;
        results.push_back(
            (co_await f.project.compile_unit(id("use.cpp"), std::move(first)))->status);
        evicted = f.project.pcms.find(id("low.cppm"))->second.path;
        ZASSERT(!evicted.empty());
        ZASSERT(fs::remove(evicted).has_value());
        UnitRequest second;
        second.index = true;
        results.push_back(
            (co_await f.project.compile_unit(id("use.cpp"), std::move(second)))->status);

        co_await f.project.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    using enum UnitResult::Status;
    ZEXPECT(results == (std::vector<UnitResult::Status>{Completed, Completed}));
    auto low = f.project.pcms.find(id("low.cppm"));
    ZASSERT(low != f.project.pcms.end());
    ZEXPECT(!low->second.path.empty());
    ZEXPECT(fs::exists(low->second.path));
}

ZEST_CASE(CrashedModuleStaysRefused) {
    // A module whose build crashed a worker stays refused while what that
    // build read holds, whatever its key: a rebuild of the PCH it reads
    // renames the key on the same bytes.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\nexport int mv() { return 1; }\n");
    tmp.touch("use.cpp", "import m;\nint u() { return mv(); }\n");

    IndexerFixture f;
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"),  {}},
                  {tmp.root, tmp.path("use.cpp"), {}},
    }));
    scan_all(f.project.cdb, f.project.toolchain, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace({.name = "pcm", .extension = ".pcm", .max_bytes = 1ull << 30});
    f.project.store.emplace(std::move(*store));

    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    auto m = id("m.cppm");
    auto read = current_input(f.project.file_table, m);
    ZASSERT(read.has_value());
    f.project.pcms[m].failure = {
        {.key = "an-earlier-key", .record = failed_build("sl:0:0", "killed by signal 11")},
        {*read}
    };

    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        UnitRequest request;
        request.index = true;
        co_await f.project.compile_unit(id("use.cpp"), std::move(request));

        co_await f.project.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    auto build = f.project.pcms.find(m);
    ZASSERT(build != f.project.pcms.end());
    ZEXPECT(build->second.failure.has_value());
    ZEXPECT(build->second.path.empty());
}

ZEST_CASE(CrashCountsOnPCH) {
    // A unit that crashes the worker over its target's precompiled header
    // runs again without it: one that parses then counts against the
    // header, one that crashes again crashed on its own code, and a unit
    // reading the header fine clears the count. Only the header's build
    // finds marker.h, so only main.cpp's parse over it crashes.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});
    TempDir tmp;
    tmp.touch("poison/marker.h", "");
    tmp.touch("pch.hxx",
              "#pragma once\n"
              "#if __has_include(\"marker.h\")\n"
              "#define POISON _Pragma(\"clang __debug crash\")\n"
              "#else\n"
              "#define POISON\n"
              "#endif\n"
              "int shared();\n");
    tmp.touch("pch.hxx.cxx", "");
    tmp.touch("main.cpp", "POISON\nint x = shared();\n");
    tmp.touch("self.cpp", "#pragma clang __debug crash\nint y = shared();\n");
    tmp.touch("ok.cpp", "int z = shared();\n");

    IndexerFixture f;
    auto header = tmp.path("pch.hxx");
    auto pch = tmp.path("pch.hxx.pch");
    std::vector<std::string> builds = {"-I",
                                       tmp.path("poison"),
                                       "-Xclang",
                                       "-emit-pch",
                                       "-Xclang",
                                       "-include",
                                       "-Xclang",
                                       header,
                                       "-x",
                                       "c++-header",
                                       "-o",
                                       pch};
    std::vector<std::string> reads =
        {"-Xclang", "-include-pch", "-Xclang", pch, "-Xclang", "-include", "-Xclang", header};
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("pch.hxx.cxx"), builds},
                  {tmp.root, tmp.path("main.cpp"),    reads },
                  {tmp.root, tmp.path("self.cpp"),    reads },
                  {tmp.root, tmp.path("ok.cpp"),      reads },
    }));
    scan_all(f.project.cdb, f.project.toolchain, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace(
        {.name = "pch", .extension = ".pch", .aux_extension = ".pch.idx", .max_bytes = 1ull << 30});
    f.project.store.emplace(std::move(*store));

    auto consume_failures = [&] {
        unsigned count = 0;
        for(auto& entry: f.project.pchs) {
            if(entry.second.consumes) {
                count += entry.second.consumes->count;
            }
        }
        return count;
    };
    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    std::vector<UnitResult::Status> results;
    std::vector<unsigned> counts;
    bool done = false;
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        for(auto name:
            {"self.cpp", "self.cpp", "main.cpp", "main.cpp", "ok.cpp", "main.cpp", "main.cpp"}) {
            UnitRequest request;
            request.index = true;
            results.push_back(
                (co_await f.project.compile_unit(id(name), std::move(request)))->status);
            counts.push_back(consume_failures());
        }

        co_await f.project.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    using enum UnitResult::Status;
    ZEXPECT(results == (std::vector<UnitResult::Status>{Preempted,
                                                        Crashed,
                                                        Preempted,
                                                        Completed,
                                                        Completed,
                                                        Preempted,
                                                        Completed}));
    ZEXPECT(counts == (std::vector<unsigned>{0, 0, 0, 1, 0, 0, 1}));
    logging::reset_anomaly_for_testing();
}

};  // ZEST_SUITE(UnitIndex)

}  // namespace
}  // namespace clice::testing
