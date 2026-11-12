module clice;

import :project.graph.pch;
import :project.graph.task_graph;
import :project.project;
import :support.logging;
import :unittests.utils.anomaly_capture;
import :unittests.utils.project_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :unittests.worker.helpers;
import :vfs.cache_store;

namespace clice::testing {
namespace {

/// The acquisition crash matrix: a build's worker death is recorded on the key
/// exactly once, whatever happens to the joiners or to the owner's own
/// frame, and the key is refused until a consumer retries it. Real workers,
/// poisoned via the crash pragma.
ZEST_SUITE(PCHAcquisition, zest::LoopFixture) {

TempDir tmp;
std::string src;
FileTable files;
WorkerPool pool{loop};
Project project{files, {}, &loop, &pool};

PCHAcquisitionTEST() {
    // Hang detection: real workers build here, slowly on a loaded machine.
    watchdog = chrono::seconds(45);
    tmp.touch("a.cpp", "");
    src = tmp.path("a.cpp");

    open_cache(tmp, project);
}

PCHRequest request(llvm::StringRef text) {
    return {
        .pch_key = "shared-key",
        .params =
            {
                     .file = src,
                     .directory = tmp.path("."),
                     .arguments = make_args(src),
                     .content = std::string(text),
                     .preamble_bound = static_cast<std::uint32_t>(text.size()),
                     },
    };
}

/// Wait for the PCH `request` names from outside the graph, foreground,
/// one attempt.
async::task<Result> acquire(PCHRequest request) {
    auto id = pch_node(request.pch_key);
    co_return co_await project.graph.join(id,
                                          project.pch_work(std::move(request)),
                                          {.foreground = true});
}

/// Run the body between pool start and graph/pool teardown.
template <typename F>
void execute(F&& fn) {
    auto body = [&]() -> async::task<> {
        ZASSERT(pool.start(worker_options(tmp, 1, 0)));

        co_await fn();

        co_await project.graph.shutdown();
        co_await pool.stop();
    };
    auto [ran] = run(body());
    ZEXPECT(ran.has_value());
}

ZEST_CASE(PoisonKeyRecordsOnce) {
    // Two acquires of one poisoned key observe the same Failed attempt; the
    // worker names the build, so its one death is attributed to it at once — no
    // resend — and recorded on the key.
    AnomalyCapture anomalies;
    std::optional<Result> a, b;

    execute([&]() -> async::task<> {
        auto poison = "#pragma clang __debug crash\n";
        auto acquire_a = [&]() -> async::task<> {
            a = co_await acquire(request(poison));
        };
        auto acquire_b = [&]() -> async::task<> {
            b = co_await acquire(request(poison));
        };
        co_await async::when_all(acquire_a(), acquire_b());
    });

    ZEXPECT(a == Result::Failed);
    ZEXPECT(b == Result::Failed);
    ZEXPECT(anomalies.count(logging::AnomalyId::WorkerCrash) == 1u);
    auto* crash = project.pch_build_crash("shared-key");
    ZASSERT(crash != nullptr);
    ZEXPECT(!crash->cause.empty());
}

ZEST_CASE(OwnerGoneStillRecords) {
    // The dispatch owner's request is cancelled right after its spawn: the
    // attempt runs to its real reply, so the death is still recorded on the key —
    // a crash is never dropped because the requester went away (a stale
    // attempt's crashes still count).
    AnomalyCapture anomalies;

    std::optional<Result> joined;
    async::cancellation_source owner_scope;

    execute([&]() -> async::task<> {
        auto poison = "#pragma clang __debug crash\n";
        auto acquire_owner = [&]() -> async::task<> {
            auto result = co_await async::with_token(acquire(request(poison)), owner_scope.token());
            ZEXPECT(result.is_cancelled());
        };
        auto acquire_joiner = [&]() -> async::task<> {
            joined = co_await acquire(request(poison));
        };
        auto cancel_owner = [&]() -> async::task<> {
            owner_scope.cancel();
            co_return;
        };
        co_await async::when_all(acquire_owner(), acquire_joiner(), cancel_owner());
    });

    ZEXPECT(joined == Result::Failed);
    ZEXPECT(project.pch_build_crash("shared-key") != nullptr);
}

ZEST_CASE(ForgetLiftsRefusal) {
    AnomalyCapture anomalies;
    std::optional<Result> refused, retried;

    execute([&]() -> async::task<> {
        auto poison = "#pragma clang __debug crash\n";
        co_await acquire(request(poison));
        // Refused before any dispatch: no second death.
        refused = co_await acquire(request(poison));
        ZASSERT(anomalies.count(logging::AnomalyId::WorkerCrash) == 1u);

        project.forget_pch_failure("shared-key");
        retried = co_await acquire(request(poison));
    });

    ZEXPECT(refused == Result::Failed);
    ZEXPECT(retried == Result::Failed);
    ZEXPECT(anomalies.count(logging::AnomalyId::WorkerCrash) == 2u);
}

ZEST_CASE(DeclaredCrashLifts) {
    // A precompiled header the build declares is keyed by its command, not
    // by the header's text: its crash is known to have read the header,
    // and a fix there builds it again.
    AnomalyCapture anomalies;
    tmp.touch("a.h", "#pragma clang __debug crash\n");
    auto header = tmp.path("a.h");
    auto whole = [&] {
        return PCHRequest{
            .pch_key = "declared-key",
            .params =
                {
                         .file = header,
                         .directory = tmp.path("."),
                         .arguments = make_args(header, "-xc++-header"),
                         .whole_file = true,
                         },
        };
    };
    std::optional<Result> refused, fixed;

    execute([&]() -> async::task<> {
        co_await acquire(whole());
        refused = co_await acquire(whole());
        ZASSERT(anomalies.count(logging::AnomalyId::WorkerCrash) == 1u);

        tmp.touch("a.h", "#define FIXED 1\n");
        fixed = co_await acquire(whole());
    });

    ZEXPECT(refused == Result::Failed);
    ZEXPECT(fixed == Result::Success);
    ZEXPECT(anomalies.count(logging::AnomalyId::WorkerCrash) == 1u);
}

ZEST_CASE(SharedBuildBothReady) {
    // The success half of the matrix: one build serves every joiner, no
    // crash is recorded, and the pair ends up registered under the key.
    std::optional<Result> a, b;

    execute([&]() -> async::task<> {
        auto text = "#define X 1\n";
        auto acquire_a = [&]() -> async::task<> {
            a = co_await acquire(request(text));
        };
        auto acquire_b = [&]() -> async::task<> {
            b = co_await acquire(request(text));
        };
        co_await async::when_all(acquire_a(), acquire_b());
    });

    ZEXPECT(a == Result::Success);
    ZEXPECT(b == Result::Success);
    ZEXPECT(project.pch_build_crash("shared-key") == nullptr);
    auto it = project.pchs.find("shared-key");
    ZASSERT(it != project.pchs.end());
    ZEXPECT(!it->second.path.empty());
    ZEXPECT(!it->second.index_path.empty());
}

ZEST_CASE(ConsumeFailuresStopKey) {
    // Consumption keeps a record of its own: the build's failure clears on
    // every successful rebuild, so a pair whose every rebuild fails to be
    // read would rebuild forever without one. A clean consumption clears
    // the record; failures in a row stop the key and acquisition fails
    // fast, before any dispatch.
    std::optional<Result> cleared, stopped;
    execute([&]() -> async::task<> {
        ZASSERT(co_await acquire(request("#define X 1\n")) == Result::Success);
        project.pch_consume_failed("shared-key");
        ZASSERT(co_await acquire(request("#define X 1\n")) == Result::Success);
        project.pch_consume_succeeded("shared-key");
        project.pch_consume_failed("shared-key");
        cleared = co_await acquire(request("#define X 1\n"));

        project.pch_consume_failed("shared-key");
        stopped = co_await acquire(request("#define X 1\n"));
    });
    ZEXPECT(cleared == Result::Success);
    ZEXPECT(stopped == Result::Failed);
}

ZEST_CASE(SharedPairFailsOnce) {
    // Two parses read the same pair and both fail to: the second finds it
    // retracted already, and the key keeps building.
    std::optional<Result> rebuilt;
    execute([&]() -> async::task<> {
        ZASSERT(co_await acquire(request("#define X 1\n")) == Result::Success);
        project.pch_consume_failed("shared-key");
        project.pch_consume_failed("shared-key");
        rebuilt = co_await acquire(request("#define X 1\n"));
    });
    ZEXPECT(rebuilt == Result::Success);
}

ZEST_CASE(EvictedPairNotCounted) {
    // A pair the store evicted while a parse read it says nothing of its
    // bytes: its consumers' failures count nothing, and the key keeps
    // building however often that happens.
    std::optional<Result> first, second;
    execute([&]() -> async::task<> {
        auto evict = [&] {
            project.store->invalidate("pch", project.pchs.find("shared-key")->second.blob);
        };
        ZASSERT(co_await acquire(request("#define X 1\n")) == Result::Success);
        evict();
        project.pch_consume_failed("shared-key");
        first = co_await acquire(request("#define X 1\n"));
        evict();
        project.pch_consume_failed("shared-key");
        second = co_await acquire(request("#define X 1\n"));
    });
    ZEXPECT(first == Result::Success);
    ZEXPECT(second == Result::Success);
}

};  // ZEST_SUITE(PCHAcquisition)

}  // namespace
}  // namespace clice::testing
