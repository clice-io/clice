#include <string>
#include <vector>

#include "test/test.h"
#include "support/anomaly.h"
#include "support/logging.h"

namespace clice::testing {
namespace {

using logging::AnomalyId;
using logging::NotifyLevel;

/// RAII fixture: install test hooks and clear them on destruction.
struct AnomalyCapture {
    std::vector<std::pair<NotifyLevel, std::string>> notified;
    std::vector<AnomalyId> trapped;
    logging::Level saved_level;

    AnomalyCapture() : saved_level(logging::options.level) {
        logging::reset_anomaly_for_testing();
        logging::set_notify_hook([this](NotifyLevel level, std::string_view message) {
            notified.emplace_back(level, std::string(message));
        });
        logging::set_anomaly_trap_for_testing([this](AnomalyId id) { trapped.push_back(id); });
    }

    ~AnomalyCapture() {
        logging::set_notify_hook(nullptr);
        logging::reset_anomaly_for_testing();
        logging::options.level = saved_level;
    }
};

TEST_SUITE(Anomaly) {

TEST_CASE(MarkerAndNotify) {
    AnomalyCapture capture;

    LOG_ANOMALY(PchBuildFail, "stale build for {}", "main.cpp");

    ASSERT_EQ(capture.notified.size(), 1u);
    auto& [level, message] = capture.notified.front();
    EXPECT_EQ(level, NotifyLevel::Error);
    EXPECT_EQ(message, "[anomaly:pch_build_fail] stale build for main.cpp");
}

TEST_CASE(TrapInvokedPerReport) {
    /// The trap fires once per reported (non-suppressed) anomaly. In Debug
    /// builds the default trap aborts the process; the override used here is
    /// the mock point that lets us observe it in any build type.
    AnomalyCapture capture;

    LOG_ANOMALY(WorkerCrash, "worker {} died", 1);
    LOG_ANOMALY(WorkerCrash, "worker {} died", 2);

    ASSERT_EQ(capture.trapped.size(), 2u);
    EXPECT_EQ(capture.trapped[0], AnomalyId::WorkerCrash);
}

TEST_CASE(RateLimitSuppresses) {
    AnomalyCapture capture;

    for(std::uint32_t i = 0; i < logging::anomaly_report_limit + 5; ++i) {
        LOG_ANOMALY(CompileFail, "occurrence {}", i);
    }

    EXPECT_EQ(capture.notified.size(), logging::anomaly_report_limit);
    EXPECT_EQ(capture.trapped.size(), logging::anomaly_report_limit);
}

TEST_CASE(RateLimitPerId) {
    AnomalyCapture capture;

    for(std::uint32_t i = 0; i < logging::anomaly_report_limit + 5; ++i) {
        LOG_ANOMALY(CompileFail, "occurrence {}", i);
    }
    LOG_ANOMALY(PcmBuildFail, "different id still reports");

    EXPECT_EQ(capture.notified.size(), logging::anomaly_report_limit + 1);
}

TEST_CASE(SuppressedArgsNotEvaluated) {
    /// Locks the lazy-evaluation contract: once the rate limit gate fails,
    /// the format arguments must not be evaluated at all.
    AnomalyCapture capture;

    int evaluations = 0;
    auto observe = [&evaluations] {
        ++evaluations;
        return 42;
    };

    for(std::uint32_t i = 0; i < logging::anomaly_report_limit + 5; ++i) {
        LOG_ANOMALY(PositionMapFail, "value {}", observe());
    }

    EXPECT_EQ(evaluations, static_cast<int>(logging::anomaly_report_limit));
}

TEST_CASE(LevelGateSkipsEvaluation) {
    AnomalyCapture capture;
    logging::options.level = logging::Level::off;

    int evaluations = 0;
    auto observe = [&evaluations] {
        ++evaluations;
        return 42;
    };
    LOG_ANOMALY(PchBuildFail, "value {}", observe());

    EXPECT_EQ(evaluations, 0);
    EXPECT_EQ(capture.notified.size(), 0u);
    EXPECT_EQ(capture.trapped.size(), 0u);
}

TEST_CASE(GuidanceMarkerAndLevel) {
    AnomalyCapture capture;

    LOG_GUIDANCE("no compilation database found in {}", "/tmp/ws");

    ASSERT_EQ(capture.notified.size(), 1u);
    auto& [level, message] = capture.notified.front();
    EXPECT_EQ(level, NotifyLevel::Warning);
    EXPECT_EQ(message, "[guidance] no compilation database found in /tmp/ws");
    EXPECT_EQ(capture.trapped.size(), 0u);
}

TEST_CASE(GuidanceLazyAtLevel) {
    AnomalyCapture capture;
    logging::options.level = logging::Level::off;

    int evaluations = 0;
    auto observe = [&evaluations] {
        ++evaluations;
        return 1;
    };
    LOG_GUIDANCE("value {}", observe());

    EXPECT_EQ(evaluations, 0);
    EXPECT_EQ(capture.notified.size(), 0u);
}

TEST_CASE(MarkerNamesStable) {
    /// Every id fires through the macro once and produces its wire marker.
    /// Integration tests grep these exact strings — keep them stable.
    AnomalyCapture capture;

    LOG_ANOMALY(PchBuildFail, "x");
    LOG_ANOMALY(PcmBuildFail, "x");
    LOG_ANOMALY(CompileFail, "x");
    LOG_ANOMALY(WorkerRequestFail, "x");
    LOG_ANOMALY(WorkerCrash, "x");
    LOG_ANOMALY(WorkerSpawnFail, "x");
    LOG_ANOMALY(PositionMapFail, "x");

    ASSERT_EQ(capture.notified.size(), logging::anomaly_id_count);
    const char* expected[] = {
        "pch_build_fail",
        "pcm_build_fail",
        "compile_fail",
        "worker_request_fail",
        "worker_crash",
        "worker_spawn_fail",
        "position_map_fail",
    };
    for(std::size_t i = 0; i < logging::anomaly_id_count; ++i) {
        EXPECT_EQ(capture.notified[i].second, std::format("[anomaly:{}] x", expected[i]));
    }
}

};  // TEST_SUITE(Anomaly)

}  // namespace
}  // namespace clice::testing
