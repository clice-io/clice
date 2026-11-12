module clice;

import :compile.compilation;
import :project.graph.pcm;
import :project.graph.precompiled;
import :project.graph.task_graph;
import :project.graph.unit;
import :settings.rules;
import :settings.settings;
import :support.logging;
import :unittests.utils.anomaly_capture;
import :unittests.utils.cdb_helper;
import :unittests.utils.index_internals;
import :unittests.utils.indexer_fixture;
import :unittests.utils.project_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :unittests.worker.helpers;
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

    open_cache(tmp, f.project);

    UnitRequest request;
    request.tidy = tidy::TidyParams{
        .checks = "-*,bugprone-integer-division",
        .extra_args = {"-DUSE_M"},
    };

    auto n_id = f.project.file_table.intern(Spelling::absolute(tmp.path("n.cppm")));
    std::optional<UnitResult> result;
    bool done = false;
    auto body = [&]() -> async::task<> {
        ZASSERT(f.pool.start(worker_options(tmp, 1, 0)));

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
    // b.cpp is told x.h's variant is stored and names it by hash alone;
    // the variant leaves the index before the result arrives. That result
    // must not commit, and the rerun sends every section.
    TempDir tmp;
    tmp.touch("x.h", "#pragma once\nint f();\n");
    tmp.touch("a.cpp", "#include \"x.h\"\nint a() { return f(); }\n");
    tmp.touch("b.cpp", "#include \"x.h\"\nint b() { return f(); }\n");

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
    auto x_id = f.project.file_table.intern(Spelling::absolute(tmp.path("x.h")));
    ZASSERT(f.project.index.shard(x_id));

    auto b_id = f.project.file_table.intern(Spelling::absolute(tmp.path("b.cpp")));
    std::vector<UnitResult::Status> results;
    bool done = false;
    auto body = [&]() -> async::task<> {
        ZASSERT(f.pool.start(worker_options(tmp, 1, 0)));

        for(int run = 0; run < 2; run += 1) {
            UnitRequest request;
            request.index = true;
            // Asked as the result arrives, before it merges.
            request.outdated = [&f, x_id, run] {
                if(run == 0) {
                    IndexInternals::retire_shard(f.project.index, x_id);
                }
                return false;
            };
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
    ZEXPECT(f.project.index.shard(x_id) != nullptr);
}

ZEST_CASE(BuildsIndexModules) {
    // A unit providing a module is indexed by its PCM build, and its run
    // compiles nothing more: the interface and an implementation partition
    // build theirs, the implementation unit provides none and compiles, over
    // the interface's PCM.
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

    open_cache(tmp, f.project);

    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    auto built_pcms = [&] {
        return ranges::count_if(f.project.pcms,
                                [](auto& entry) { return !entry.second.path.empty(); });
    };
    std::vector<UnitResult::Status> results;
    std::vector<std::size_t> compiled;
    std::size_t pcms_before_impl = 0;
    bool done = false;
    auto body = [&]() -> async::task<> {
        ZASSERT(f.pool.start(worker_options(tmp, 1, 0)));

        for(auto name: {"m.cppm", "detail.cpp", "impl.cpp"}) {
            if(llvm::StringRef(name) == "impl.cpp") {
                pcms_before_impl = built_pcms();
            }
            UnitRequest request;
            request.index = true;
            auto result = co_await f.project.compile_unit(id(name), std::move(request));
            results.push_back(result->status);
            compiled.push_back(result->perf.bytes);
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
    ZEXPECT(pcms_before_impl == 2u);
    ZEXPECT(built_pcms() == 2);
    ZEXPECT(compiled[0] == 0u);
    ZEXPECT(compiled[1] == 0u);
    ZEXPECT(compiled[2] != 0u);
    for(auto name: {"m.cppm", "detail.cpp", "impl.cpp"}) {
        ZEXPECT(f.project.index.shard(id(name)) != nullptr);
    }
}

ZEST_CASE(EntryBuildLeavesIndex) {
    // An importer under C++23 reads the module's build under its C++23
    // entry, which leaves the module's index to the build under its own
    // command.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\nexport long mv() { return __cplusplus; }\n");
    tmp.touch("newer.cpp", "import m;\nlong use() { return mv(); }\n");

    IndexerFixture f;
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"),    {}            },
                  {tmp.root, tmp.path("m.cppm"),    {"-std=c++23"}},
                  {tmp.root, tmp.path("newer.cpp"), {"-std=c++23"}},
    }));
    scan_all(f.project.cdb, f.project.toolchain, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    open_cache(tmp, f.project);

    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    std::optional<UnitResult::Status> result;
    bool done = false;
    auto body = [&]() -> async::task<> {
        ZASSERT(f.pool.start(worker_options(tmp, 1, 0)));

        UnitRequest request;
        request.index = true;
        result = (co_await f.project.compile_unit(id("newer.cpp"), std::move(request)))->status;

        co_await f.project.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    ZEXPECT(result == UnitResult::Status::Completed);
    ZASSERT(f.project.pcms.size() == 1u);
    auto& [module, build] = *f.project.pcms.begin();
    ZEXPECT(module_context(module) != 0u);
    ZEXPECT(!build.path.empty());
    ZEXPECT(f.project.index.shard(id("m.cppm")) == nullptr);
    ZEXPECT(f.project.index.shard(id("newer.cpp")) != nullptr);
}

ZEST_CASE(ExcludedModuleStillIndexed) {
    // A module unit a rule keeps out of the index is still indexed by the
    // PCM build its importer waits on, as a header the rule covers is by
    // its includers, and that index survives the next load.
    TempDir tmp;
    tmp.touch("vendor/m.cppm", "export module m;\nexport int mv() { return 1; }\n");
    tmp.touch("use.cpp", "import m;\nint u() { return mv(); }\n");

    auto setup = [&](IndexerFixture& f) {
        open_store(tmp, f.project);
        write_cdb(tmp,
                  f.project.cdb,
                  build_cdb_json({
                      {tmp.root, tmp.path("vendor/m.cppm"), {}},
                      {tmp.root, tmp.path("use.cpp"),       {}},
        }));
        f.project.settings.rules.push_back(Rule{.patterns = {"vendor/**"}, .index = false});
        f.project.root = CanonicalPath(Spelling::absolute(tmp.root));
        f.project.settings.finalize(f.project.root);
        f.project.rules = Rules(f.project.settings, f.project.root, "");
        scan_all(f.project.cdb, f.project.toolchain, f.project.dep_graph);
        f.project.dep_graph.build_reverse_map();
    };
    auto id = [&](IndexerFixture& f, llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };

    {
        IndexerFixture f;
        setup(f);
        ZASSERT(!f.project.rules.indexable(f.project.file_table.resolve(id(f, "vendor/m.cppm"))));
        std::optional<UnitResult::Status> status;
        bool done = false;
        auto body = [&]() -> async::task<> {
            ZASSERT(f.pool.start(worker_options(tmp, 1, 0)));

            UnitRequest request;
            request.index = true;
            status =
                (co_await f.project.compile_unit(id(f, "use.cpp"), std::move(request)))->status;

            co_await f.project.graph.shutdown();
            co_await f.pool.stop();
            done = true;
        };
        auto task = body();
        f.loop.schedule(task);
        f.loop.run();
        ZEXPECT(done);
        ZEXPECT(status == UnitResult::Status::Completed);
        ZEXPECT(f.project.index.shard(id(f, "vendor/m.cppm")) != nullptr);
        f.save();
    }

    IndexerFixture f;
    setup(f);
    f.load();
    ZEXPECT(f.project.index.indexed(id(f, "vendor/m.cppm")));
}

ZEST_CASE(BrokenModuleCompilesItself) {
    // A module interface whose PCM fails to build on its errors is indexed
    // by its own compile, in spite of them.
    TempDir tmp;
    tmp.touch("m.cppm", "export module m;\nexport int mv() { return undeclared; }\nint ok();\n");

    IndexerFixture f;
    write_cdb(tmp,
              f.project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("m.cppm"), {}},
    }));
    scan_all(f.project.cdb, f.project.toolchain, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    open_cache(tmp, f.project);

    auto id = f.project.file_table.intern(Spelling::absolute(tmp.path("m.cppm")));
    std::optional<UnitResult> result;
    bool done = false;
    auto body = [&]() -> async::task<> {
        ZASSERT(f.pool.start(worker_options(tmp, 1, 0)));

        UnitRequest request;
        request.index = true;
        result = co_await f.project.compile_unit(id, std::move(request));

        co_await f.project.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    ZASSERT(result.has_value());
    ZEXPECT(result->status == UnitResult::Status::Completed);
    ZEXPECT(result->perf.bytes != 0u);
    ZEXPECT(f.project.built_pcm(module_node(id)) == nullptr);
    ZEXPECT(f.project.index.shard(id) != nullptr);
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

    open_cache(tmp, f.project);

    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    llvm::SmallVector<NodeId> modules{module_node(id("bad.cppm")), module_node(id("good.cppm"))};
    llvm::SmallVector<NodeId> failed;
    std::optional<Result> waited;
    bool done = false;
    auto body = [&]() -> async::task<> {
        ZASSERT(f.pool.start(worker_options(tmp, 1, 0)));

        auto heard = [&](NodeId module) {
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
    ZEXPECT(failed == llvm::SmallVector<NodeId>{module_node(id("bad.cppm"))});
    ZEXPECT(f.project.built_pcm(module_node(id("good.cppm"))) != nullptr);
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

    open_cache(tmp, f.project);

    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    std::vector<UnitResult::Status> results;
    std::string evicted;
    bool done = false;
    auto body = [&]() -> async::task<> {
        ZASSERT(f.pool.start(worker_options(tmp, 1, 0)));

        UnitRequest first;
        first.index = true;
        results.push_back(
            (co_await f.project.compile_unit(id("use.cpp"), std::move(first)))->status);
        evicted = f.project.pcms.find(module_node(id("low.cppm")))->second.path;
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
    auto low = f.project.pcms.find(module_node(id("low.cppm")));
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

    open_cache(tmp, f.project);

    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    auto m = id("m.cppm");
    auto read = current_input(f.project.file_table, m);
    ZASSERT(read.has_value());
    f.project.pcms[module_node(m)].failure = {
        {.key = "an-earlier-key", .record = failed_build("sl:0:0", "killed by signal 11")},
        {*read}
    };

    bool done = false;
    auto body = [&]() -> async::task<> {
        ZASSERT(f.pool.start(worker_options(tmp, 1, 0)));

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

    auto build = f.project.pcms.find(module_node(m));
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
    AnomalyCapture anomalies;
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

    open_cache(tmp, f.project);

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
        ZASSERT(f.pool.start(worker_options(tmp, 1, 0)));

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
}

ZEST_CASE(PCHBuildIndexesWriter) {
    // The build of a precompiled header a target declares is the compile of
    // the unit writing it: the header's declarations are indexed with that
    // unit by the build a unit loading the header waits on, and the
    // writer's own run compiles nothing more.
    TempDir tmp;
    tmp.touch("pch.hxx", "#pragma once\nint shared();\n");
    tmp.touch("pch.hxx.cxx", "");
    tmp.touch("use.cpp", "int x = shared();\n");

    IndexerFixture f;
    auto header = tmp.path("pch.hxx");
    auto pch = tmp.path("pch.hxx.pch");
    std::vector<std::string> builds = {"-Xclang",
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
                  {tmp.root, tmp.path("use.cpp"),     reads },
    }));
    scan_all(f.project.cdb, f.project.toolchain, f.project.dep_graph);
    f.project.dep_graph.build_reverse_map();

    open_cache(tmp, f.project);

    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    std::vector<UnitResult::Status> results;
    std::vector<std::size_t> compiled;
    bool writer_indexed = false;
    bool header_indexed = false;
    bool done = false;
    auto body = [&]() -> async::task<> {
        ZASSERT(f.pool.start(worker_options(tmp, 1, 0)));

        for(auto name: {"use.cpp", "pch.hxx.cxx"}) {
            UnitRequest request;
            request.index = true;
            auto result = co_await f.project.compile_unit(id(name), std::move(request));
            results.push_back(result->status);
            compiled.push_back(result->perf.bytes);
            if(llvm::StringRef(name) == "use.cpp") {
                writer_indexed = f.project.index.indexed(id("pch.hxx.cxx"));
                header_indexed = f.project.index.shard(id("pch.hxx")) != nullptr;
            }
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
    ZEXPECT(results == (std::vector<UnitResult::Status>{Completed, Completed}));
    ZEXPECT(writer_indexed);
    ZEXPECT(header_indexed);
    ZEXPECT(compiled[0] != 0u);
    ZEXPECT(compiled[1] == 0u);
    ZEXPECT(ranges::count_if(f.project.pchs, [](auto& entry) {
                return entry.second.whole_file && !entry.second.path.empty();
            }) == 1);
}

};  // ZEST_SUITE(UnitIndex)

}  // namespace
}  // namespace clice::testing
