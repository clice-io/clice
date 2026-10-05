#include <optional>
#include <string>

#include "test/temp_dir.h"
#include "test/test.h"
#include "sched/families/pch.h"
#include "server/worker_test_helpers.h"
#include "support/anomaly.h"
#include "support/cache_store.h"

#include "kota/zest/async.h"

namespace clice::testing {
namespace {

/// The acquisition crash matrix: a build's worker death lands on the key
/// exactly once, whatever happens to the joiners or to the owner's own
/// frame, and the key is refused until a consumer forgives it. Real
/// workers, poisoned via the crash pragma.
ZEST_SUITE(PCHFamilyAcquisition, kota::zest::LoopFixture) {

TempDir tmp;
std::string src;
FileTable files;
Project project{files};
WorkerPool pool{loop};
TaskGraph graph;
PCHFamily pch{graph, project, pool};

PCHFamilyAcquisitionTEST() {
    tmp.touch("a.cpp", "");
    src = tmp.path("a.cpp");

    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace({.name = "pch",
                               .extension = ".pch",
                               .aux_extension = ".pch.idx",
                               .policy = CachePolicy::LRU,
                               .max_bytes = 1ull << 30});
    project.store.emplace(std::move(*store));
    pch.register_runner();
}

PCHFamily::Request request(llvm::StringRef text) {
    return {
        .pch_key = "shared-key",
        .file = src,
        .directory = tmp.path("."),
        .arguments = make_args(src),
        .content = std::string(text),
        .preamble_bound = static_cast<std::uint32_t>(text.size()),
    };
}

/// Run the body between pool start and graph/pool teardown.
template <typename F>
void execute(F&& fn) {
    auto body = [&]() -> kota::task<> {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = 1;
        opts.stateful_count = 0;
        ZASSERT(pool.start(opts));

        co_await fn();

        co_await graph.shutdown();
        co_await pool.stop();
    };
    auto [ran] = run(body());
    ZEXPECT(ran.has_value());
}

ZEST_CASE(PoisonKeyRecordsOnce) {
    // Two acquires of one poisoned key observe the same Failed round; the
    // worker names the build, so its one death is blamed at once — no
    // resend — and recorded on the key.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    int deaths = 0;
    pool.on_crash = [&](const WorkerCrashInfo&) {
        deaths += 1;
    };
    std::optional<PCHFamily::Outcome> a, b;

    execute([&]() -> kota::task<> {
        auto poison = "#pragma clang __debug crash\n";
        auto acquire_a = [&]() -> kota::task<> {
            a = co_await pch.acquire(request(poison));
        };
        auto acquire_b = [&]() -> kota::task<> {
            b = co_await pch.acquire(request(poison));
        };
        co_await kota::when_all(acquire_a(), acquire_b());
    });

    ZEXPECT(a == PCHFamily::Outcome::Failed);
    ZEXPECT(b == PCHFamily::Outcome::Failed);
    ZEXPECT(deaths == 1);
    auto* crash = pch.crashed("shared-key");
    ZASSERT(crash != nullptr);
    ZEXPECT(crash->code == worker::dispatch_errc::worker_crashed);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(OwnerGoneStillRecords) {
    // The dispatch owner's request is cancelled right after its spawn: the
    // round runs to its real reply, so the death still lands on the key —
    // a crash is never dropped because the requester went away (a stale
    // round's crashes still count).
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    std::optional<PCHFamily::Outcome> joined;
    kota::cancellation_source owner_scope;

    execute([&]() -> kota::task<> {
        auto poison = "#pragma clang __debug crash\n";
        auto acquire_owner = [&]() -> kota::task<> {
            auto result =
                co_await kota::with_token(pch.acquire(request(poison)), owner_scope.token());
            ZEXPECT(result.is_cancelled());
        };
        auto acquire_joiner = [&]() -> kota::task<> {
            joined = co_await pch.acquire(request(poison));
        };
        auto cancel_owner = [&]() -> kota::task<> {
            owner_scope.cancel();
            co_return;
        };
        co_await kota::when_all(acquire_owner(), acquire_joiner(), cancel_owner());
    });

    ZEXPECT(joined == PCHFamily::Outcome::Failed);
    ZEXPECT(pch.crashed("shared-key") != nullptr);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(ForgiveLiftsRefusal) {
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    int deaths = 0;
    pool.on_crash = [&](const WorkerCrashInfo&) {
        deaths += 1;
    };
    std::optional<PCHFamily::Outcome> refused, retried;

    execute([&]() -> kota::task<> {
        auto poison = "#pragma clang __debug crash\n";
        co_await pch.acquire(request(poison));
        // Refused before any dispatch: no second death.
        refused = co_await pch.acquire(request(poison));
        ZASSERT(deaths == 1);

        pch.forgive("shared-key");
        retried = co_await pch.acquire(request(poison));
    });

    ZEXPECT(refused == PCHFamily::Outcome::Failed);
    ZEXPECT(retried == PCHFamily::Outcome::Failed);
    ZEXPECT(deaths == 2);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(SharedBuildBothReady) {
    // The success half of the matrix: one build serves every joiner, no
    // crash is recorded, and the pair lands registered under the key.
    std::optional<PCHFamily::Outcome> a, b;

    execute([&]() -> kota::task<> {
        auto text = "#define X 1\n";
        auto acquire_a = [&]() -> kota::task<> {
            a = co_await pch.acquire(request(text));
        };
        auto acquire_b = [&]() -> kota::task<> {
            b = co_await pch.acquire(request(text));
        };
        co_await kota::when_all(acquire_a(), acquire_b());
    });

    ZEXPECT(a == PCHFamily::Outcome::Ready);
    ZEXPECT(b == PCHFamily::Outcome::Ready);
    ZEXPECT(pch.crashed("shared-key") == nullptr);
    auto it = project.pch_cache.find("shared-key");
    ZASSERT(it != project.pch_cache.end());
    ZEXPECT(!it->second.path.empty());
    ZEXPECT(!it->second.index_path.empty());
}

ZEST_CASE(BlameParksKey) {
    // Consumption strikes keep their own ledger: the build-side budget
    // clears on every successful rebuild, so a pair whose every rebuild
    // gets blamed again would rebuild forever without one. A clean
    // consumption clears the strikes; consecutive blames park the key
    // and acquisition fails fast, before any dispatch.
    std::optional<PCHFamily::Outcome> cleared, parked;
    execute([&]() -> kota::task<> {
        pch.blame("shared-key");
        pch.consumed_ok("shared-key");
        pch.blame("shared-key");
        cleared = co_await pch.acquire(request("#define X 1\n"));

        pch.blame("shared-key");
        parked = co_await pch.acquire(request("#define X 1\n"));
    });
    ZEXPECT(cleared == PCHFamily::Outcome::Ready);
    ZEXPECT(parked == PCHFamily::Outcome::Failed);
}

};  // ZEST_SUITE(PCHFamilyAcquisition)

}  // namespace
}  // namespace clice::testing
