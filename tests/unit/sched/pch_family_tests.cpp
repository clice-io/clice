#include <optional>
#include <string>

#include "test/temp_dir.h"
#include "test/test.h"
#include "sched/families/pch.h"
#include "server/worker_test_helpers.h"
#include "support/anomaly.h"
#include "support/cache_store.h"

namespace clice::testing {
namespace {

/// The acquisition evidence matrix: worker deaths land exactly once, on
/// the dispatch owner's probe, whatever happens to the joiners or to the
/// owner's own frame. Real workers, poisoned via the crash pragma.
TEST_SUITE(PCHFamilyAcquisition) {

std::optional<TempDir> tmp;
std::string src;
std::optional<kota::event_loop> loop;
std::optional<Workspace> workspace;
std::optional<ContextResolver> contexts;
std::optional<WorkerPool> pool;
std::optional<TaskGraph> graph;
std::optional<PCHFamily> pch;

void setup() {
    tmp.emplace();
    tmp->touch("a.cpp", "");
    src = tmp->path("a.cpp");

    loop.emplace();
    workspace.emplace();
    auto store = CacheStore::open(tmp->path("root"), 1);
    ASSERT_TRUE(store.has_value());
    store->register_namespace({.name = "pch",
                               .extension = ".pch",
                               .aux_extension = ".pch.idx",
                               .policy = CachePolicy::LRU,
                               .max_bytes = 1ull << 30});
    workspace->store.emplace(std::move(*store));

    contexts.emplace(*workspace);
    pool.emplace(*loop);
    graph.emplace(*loop);
    pch.emplace(*graph, *workspace, *contexts, *pool);
    pch->register_runner();
}

PCHFamily::Request request(llvm::StringRef text) {
    return {
        .pch_key = "shared-key",
        .file = src,
        .directory = tmp->path("."),
        .arguments = make_args(src),
        .content = std::string(text),
        .preamble_bound = static_cast<std::uint32_t>(text.size()),
    };
}

/// Run the body between pool start and graph/pool teardown.
template <typename F>
void execute(F&& fn) {
    bool done = false;
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        CO_ASSERT_TRUE(pool->start(opts));
        co_await kota::sleep(500);

        co_await fn();

        co_await graph->shutdown();
        co_await pool->stop();
        done = true;
    };
    auto task = body();
    loop->schedule(task);
    loop->run();
    EXPECT_TRUE(done);
}

TEST_CASE(JoinerNoReplay) {
    // Two acquires of one poisoned key: the spawner's probe collects both
    // worker deaths (one per killed worker, the retry's death is separate
    // evidence); the joiner observes the same Failed round but never
    // replays the deaths — alternating replays would break quarantine's
    // adjacent-death dedup and double-count.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});
    setup();

    int owner_deaths = 0;
    int joiner_deaths = 0;
    std::optional<PCHFamily::Outcome> a, b;

    execute([&]() -> kota::task<> {
        auto poison = "#pragma clang __debug crash\n";
        auto acquire_a = [&]() -> kota::task<> {
            a = co_await pch->acquire(request(poison), [&](llvm::StringRef) { owner_deaths += 1; });
        };
        auto acquire_b = [&]() -> kota::task<> {
            b = co_await pch->acquire(request(poison),
                                      [&](llvm::StringRef) { joiner_deaths += 1; });
        };
        co_await kota::when_all(acquire_a(), acquire_b());
    });

    EXPECT_TRUE(a == PCHFamily::Outcome::Failed);
    EXPECT_TRUE(b == PCHFamily::Outcome::Failed);
    EXPECT_EQ(owner_deaths, 2);
    EXPECT_EQ(joiner_deaths, 0);

    logging::reset_anomaly_for_testing();
}

TEST_CASE(OwnerGoneStillRecords) {
    // The dispatch owner's request is cancelled right after its spawn:
    // the round holds the probe and runs to its real reply, so both
    // deaths still land on the owner — evidence is never dropped because
    // the requester went away (a stale round's crashes still count).
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});
    setup();

    int owner_deaths = 0;
    int joiner_deaths = 0;
    std::optional<PCHFamily::Outcome> joined;
    kota::cancellation_source owner_scope;

    execute([&]() -> kota::task<> {
        auto poison = "#pragma clang __debug crash\n";
        auto acquire_owner = [&]() -> kota::task<> {
            auto result = co_await kota::with_token(
                pch->acquire(request(poison), [&](llvm::StringRef) { owner_deaths += 1; }),
                owner_scope.token());
            EXPECT_TRUE(result.is_cancelled());
        };
        auto acquire_joiner = [&]() -> kota::task<> {
            joined = co_await pch->acquire(request(poison),
                                           [&](llvm::StringRef) { joiner_deaths += 1; });
        };
        auto cancel_owner = [&]() -> kota::task<> {
            owner_scope.cancel();
            co_return;
        };
        co_await kota::when_all(acquire_owner(), acquire_joiner(), cancel_owner());
    });

    EXPECT_TRUE(joined == PCHFamily::Outcome::Failed);
    EXPECT_EQ(owner_deaths, 2);
    EXPECT_EQ(joiner_deaths, 0);

    logging::reset_anomaly_for_testing();
}

TEST_CASE(SharedBuildBothReady) {
    // The success half of the matrix: one build serves every joiner, no
    // probe fires, and the pair lands registered under the key.
    setup();

    int owner_deaths = 0;
    int joiner_deaths = 0;
    std::optional<PCHFamily::Outcome> a, b;

    execute([&]() -> kota::task<> {
        auto text = "#define X 1\n";
        auto acquire_a = [&]() -> kota::task<> {
            a = co_await pch->acquire(request(text), [&](llvm::StringRef) { owner_deaths += 1; });
        };
        auto acquire_b = [&]() -> kota::task<> {
            b = co_await pch->acquire(request(text), [&](llvm::StringRef) { joiner_deaths += 1; });
        };
        co_await kota::when_all(acquire_a(), acquire_b());
    });

    EXPECT_TRUE(a == PCHFamily::Outcome::Ready);
    EXPECT_TRUE(b == PCHFamily::Outcome::Ready);
    EXPECT_EQ(owner_deaths, 0);
    EXPECT_EQ(joiner_deaths, 0);
    auto it = workspace->pch_cache.find("shared-key");
    ASSERT_TRUE(it != workspace->pch_cache.end());
    EXPECT_FALSE(it->second.path.empty());
    EXPECT_FALSE(it->second.index_path.empty());
}

TEST_CASE(BlameParksKey) {
    // Consumption strikes keep their own ledger: the build-side budget
    // clears on every successful rebuild, so a pair whose every rebuild
    // gets blamed again would rebuild forever without one. A clean
    // consumption clears the strikes; consecutive blames park the key
    // and acquisition fails fast, before any dispatch.
    setup();

    std::optional<PCHFamily::Outcome> cleared, parked;
    execute([&]() -> kota::task<> {
        pch->blame("shared-key");
        pch->consumed_ok("shared-key");
        pch->blame("shared-key");
        cleared = co_await pch->acquire(request("#define X 1\n"), {});

        pch->blame("shared-key");
        parked = co_await pch->acquire(request("#define X 1\n"), {});
    });
    EXPECT_TRUE(cleared == PCHFamily::Outcome::Ready);
    EXPECT_TRUE(parked == PCHFamily::Outcome::Failed);
}

};  // TEST_SUITE(PCHFamilyAcquisition)

}  // namespace
}  // namespace clice::testing
