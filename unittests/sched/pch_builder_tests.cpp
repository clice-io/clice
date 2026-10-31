module clice;

import :sched.builders.pch;
import :support.anomaly;
import :unittests.server.worker_test_helpers;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.cache_store;

namespace clice::testing {
namespace {

/// The acquisition crash matrix: a build's worker death is recorded on the key
/// exactly once, whatever happens to the joiners or to the owner's own
/// frame, and the key is refused until a consumer retries it. Real workers,
/// poisoned via the crash pragma.
ZEST_SUITE(PCHBuilderAcquisition, kotatsu::zest::LoopFixture) {

TempDir tmp;
std::string src;
FileTable files;
Project project{files};
WorkerPool pool{loop};
TaskGraph graph;
PCHBuilder pch{graph, project, pool};

PCHBuilderAcquisitionTEST() {
    // Hang detection: real workers build here, slowly on a loaded machine.
    watchdog = std::chrono::seconds(45);
    tmp.touch("a.cpp", "");
    src = tmp.path("a.cpp");

    auto store = CacheStore::open(tmp.path("root"), 1);
    ZASSERT(store);
    store->register_namespace(
        {.name = "pch", .extension = ".pch", .aux_extension = ".pch.idx", .max_bytes = 1ull << 30});
    project.store.emplace(std::move(*store));
    pch.register_runner();
}

PCHBuilder::Request request(llvm::StringRef text) {
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
    auto body = [&]() -> async::task<> {
        WorkerPoolOptions opts;
        opts.program = clice_binary();
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
    // worker names the build, so its one death is attributed to it at once — no
    // resend — and recorded on the key.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    int deaths = 0;
    pool.on_crash = [&](const WorkerCrashInfo&) {
        deaths += 1;
    };
    std::optional<PCHBuilder::Outcome> a, b;

    execute([&]() -> async::task<> {
        auto poison = "#pragma clang __debug crash\n";
        auto acquire_a = [&]() -> async::task<> {
            a = co_await pch.acquire(request(poison));
        };
        auto acquire_b = [&]() -> async::task<> {
            b = co_await pch.acquire(request(poison));
        };
        co_await async::when_all(acquire_a(), acquire_b());
    });

    ZEXPECT(a == PCHBuilder::Outcome::Failed);
    ZEXPECT(b == PCHBuilder::Outcome::Failed);
    ZEXPECT(deaths == 1);
    auto* crash = pch.crashed("shared-key");
    ZASSERT(crash != nullptr);
    ZEXPECT(!crash->cause.empty());

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(OwnerGoneStillRecords) {
    // The dispatch owner's request is cancelled right after its spawn: the
    // round runs to its real reply, so the death is still recorded on the key —
    // a crash is never dropped because the requester went away (a stale
    // round's crashes still count).
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    std::optional<PCHBuilder::Outcome> joined;
    async::cancellation_source owner_scope;

    execute([&]() -> async::task<> {
        auto poison = "#pragma clang __debug crash\n";
        auto acquire_owner = [&]() -> async::task<> {
            auto result =
                co_await async::with_token(pch.acquire(request(poison)), owner_scope.token());
            ZEXPECT(result.is_cancelled());
        };
        auto acquire_joiner = [&]() -> async::task<> {
            joined = co_await pch.acquire(request(poison));
        };
        auto cancel_owner = [&]() -> async::task<> {
            owner_scope.cancel();
            co_return;
        };
        co_await async::when_all(acquire_owner(), acquire_joiner(), cancel_owner());
    });

    ZEXPECT(joined == PCHBuilder::Outcome::Failed);
    ZEXPECT(pch.crashed("shared-key") != nullptr);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(ForgetLiftsRefusal) {
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    int deaths = 0;
    pool.on_crash = [&](const WorkerCrashInfo&) {
        deaths += 1;
    };
    std::optional<PCHBuilder::Outcome> refused, retried;

    execute([&]() -> async::task<> {
        auto poison = "#pragma clang __debug crash\n";
        co_await pch.acquire(request(poison));
        // Refused before any dispatch: no second death.
        refused = co_await pch.acquire(request(poison));
        ZASSERT(deaths == 1);

        pch.forget_failure("shared-key");
        retried = co_await pch.acquire(request(poison));
    });

    ZEXPECT(refused == PCHBuilder::Outcome::Failed);
    ZEXPECT(retried == PCHBuilder::Outcome::Failed);
    ZEXPECT(deaths == 2);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(DeclaredCrashLifts) {
    // A precompiled header the build declares is keyed by its command, not
    // by the header's text: its crash is known to have read the header,
    // and a fix there builds it again.
    logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});

    int deaths = 0;
    pool.on_crash = [&](const WorkerCrashInfo&) {
        deaths += 1;
    };
    tmp.touch("a.h", "#pragma clang __debug crash\n");
    auto header = tmp.path("a.h");
    auto whole = [&] {
        return PCHBuilder::Request{
            .pch_key = "declared-key",
            .file = header,
            .directory = tmp.path("."),
            .arguments = make_args(header, "-xc++-header"),
            .whole_file = true,
        };
    };
    std::optional<PCHBuilder::Outcome> refused, fixed;

    execute([&]() -> async::task<> {
        co_await pch.acquire(whole());
        refused = co_await pch.acquire(whole());
        ZASSERT(deaths == 1);

        tmp.touch("a.h", "#define FIXED 1\n");
        fixed = co_await pch.acquire(whole());
    });

    ZEXPECT(refused == PCHBuilder::Outcome::Failed);
    ZEXPECT(fixed == PCHBuilder::Outcome::Ready);
    ZEXPECT(deaths == 1);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(SharedBuildBothReady) {
    // The success half of the matrix: one build serves every joiner, no
    // crash is recorded, and the pair ends up registered under the key.
    std::optional<PCHBuilder::Outcome> a, b;

    execute([&]() -> async::task<> {
        auto text = "#define X 1\n";
        auto acquire_a = [&]() -> async::task<> {
            a = co_await pch.acquire(request(text));
        };
        auto acquire_b = [&]() -> async::task<> {
            b = co_await pch.acquire(request(text));
        };
        co_await async::when_all(acquire_a(), acquire_b());
    });

    ZEXPECT(a == PCHBuilder::Outcome::Ready);
    ZEXPECT(b == PCHBuilder::Outcome::Ready);
    ZEXPECT(pch.crashed("shared-key") == nullptr);
    auto it = project.pch_cache.find("shared-key");
    ZASSERT(it != project.pch_cache.end());
    ZEXPECT(!it->second.path.empty());
    ZEXPECT(!it->second.index_path.empty());
}

ZEST_CASE(ConsumeFailuresStopKey) {
    // Consumption keeps a record of its own: the build's failure clears on
    // every successful rebuild, so a pair whose every rebuild fails to be
    // read would rebuild forever without one. A clean consumption clears
    // the record; failures in a row stop the key and acquisition fails
    // fast, before any dispatch.
    std::optional<PCHBuilder::Outcome> cleared, stopped;
    execute([&]() -> async::task<> {
        ZASSERT(co_await pch.acquire(request("#define X 1\n")) == PCHBuilder::Outcome::Ready);
        pch.consume_failed("shared-key");
        ZASSERT(co_await pch.acquire(request("#define X 1\n")) == PCHBuilder::Outcome::Ready);
        pch.consume_succeeded("shared-key");
        pch.consume_failed("shared-key");
        cleared = co_await pch.acquire(request("#define X 1\n"));

        pch.consume_failed("shared-key");
        stopped = co_await pch.acquire(request("#define X 1\n"));
    });
    ZEXPECT(cleared == PCHBuilder::Outcome::Ready);
    ZEXPECT(stopped == PCHBuilder::Outcome::Failed);
}

ZEST_CASE(SharedPairFailsOnce) {
    // Two parses read the same pair and both fail to: the second finds it
    // retracted already, and the key keeps building.
    std::optional<PCHBuilder::Outcome> rebuilt;
    execute([&]() -> async::task<> {
        ZASSERT(co_await pch.acquire(request("#define X 1\n")) == PCHBuilder::Outcome::Ready);
        pch.consume_failed("shared-key");
        pch.consume_failed("shared-key");
        rebuilt = co_await pch.acquire(request("#define X 1\n"));
    });
    ZEXPECT(rebuilt == PCHBuilder::Outcome::Ready);
}

ZEST_CASE(EvictedPairNotCounted) {
    // A pair the store evicted while a parse read it says nothing of its
    // bytes: its consumers' failures count nothing, and the key keeps
    // building however often that happens.
    std::optional<PCHBuilder::Outcome> first, second;
    execute([&]() -> async::task<> {
        auto evict = [&] {
            project.store->invalidate("pch", project.pch_cache.find("shared-key")->second.blob);
        };
        ZASSERT(co_await pch.acquire(request("#define X 1\n")) == PCHBuilder::Outcome::Ready);
        evict();
        pch.consume_failed("shared-key");
        first = co_await pch.acquire(request("#define X 1\n"));
        evict();
        pch.consume_failed("shared-key");
        second = co_await pch.acquire(request("#define X 1\n"));
    });
    ZEXPECT(first == PCHBuilder::Outcome::Ready);
    ZEXPECT(second == PCHBuilder::Outcome::Ready);
}

};  // ZEST_SUITE(PCHBuilderAcquisition)

}  // namespace
}  // namespace clice::testing
