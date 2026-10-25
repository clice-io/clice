module;

#include "support/prelude.h"

module clice;

import :compile.compilation;
import :sched.families.turun;
import :unittests.server.worker_test_helpers;
import :unittests.utils.cdb_helper;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.cache_store;
import :vfs.path;
import :worker.pool;

namespace clice::testing {
namespace {

ZEST_SUITE(TURunLint) {

ZEST_CASE(ModuleLintScanParity) {
    // Only a scan under the lint round's extra args discovers the import
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

    f.pcm.register_runner();

    TURunFamily::Plan plan;
    plan.tidy = tidy::TidyParams{
        .checks = "-*,bugprone-integer-division",
        .extra_args = {"-DUSE_M"},
    };

    auto n_id = f.project.file_table.intern(Spelling::absolute(tmp.path("n.cppm")));
    TURunFamily::Outcome outcome;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        outcome = co_await f.turun.run(n_id, std::move(plan), {});

        co_await f.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    ZEXPECT(outcome.result == TURunFamily::Result::Completed);
    // The finding inside the gated region proves the parse saw the
    // extras and consumed the edge-built PCM.
    ZASSERT(!outcome.tidy_diagnostics.empty());
    ZEXPECT(outcome.tidy_diagnostics[0].check == "bugprone-integer-division");
}

};  // ZEST_SUITE(TURunLint)

ZEST_SUITE(TURunIndex) {

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
    std::vector<TURunFamily::Result> results;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        for(int run = 0; run < 2; run += 1) {
            TURunFamily::Plan plan;
            plan.index = true;
            results.push_back((co_await f.turun.run(b_id, std::move(plan), {})).result);
        }

        co_await f.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    using enum TURunFamily::Result;
    ZEXPECT(results == (std::vector<TURunFamily::Result>{Preempted, Completed}));
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
    f.pcm.register_runner();

    auto id = [&](llvm::StringRef name) {
        return f.project.file_table.intern(Spelling::absolute(tmp.path(name)));
    };
    std::vector<TURunFamily::Result> results;
    std::size_t pcms_before_impl = 0;
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(f.pool.start(opts));

        for(auto name: {"m.cppm", "detail.cpp", "impl.cpp"}) {
            if(llvm::StringRef(name) == "impl.cpp") {
                pcms_before_impl = f.project.pcm_builds.size();
            }
            TURunFamily::Plan plan;
            plan.index = true;
            results.push_back((co_await f.turun.run(id(name), std::move(plan), {})).result);
        }

        co_await f.graph.shutdown();
        co_await f.pool.stop();
        done = true;
    };
    auto task = body();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(done);

    using enum TURunFamily::Result;
    ZEXPECT(results == (std::vector<TURunFamily::Result>{Completed, Completed, Completed}));
    ZEXPECT(pcms_before_impl == 0u);
    ZEXPECT(f.project.pcm_builds.size() == 1u);
    auto build = f.project.pcm_builds.find(id("m.cppm"));
    ZASSERT(build != f.project.pcm_builds.end());
    ZEXPECT(!build->second.path.empty());
}

};  // ZEST_SUITE(TURunIndex)

}  // namespace
}  // namespace clice::testing
