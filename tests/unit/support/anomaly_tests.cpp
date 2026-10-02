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

ZEST_SUITE(Anomaly) {

ZEST_CASE(MarkerAndNotify) {
    AnomalyCapture capture;

    LOG_ANOMALY(PCHBuildFail, "stale build for {}", "main.cpp");

    ASSERT(capture.notified.size() == 1u);
    auto& [level, message] = capture.notified.front();
    EXPECT(level == NotifyLevel::Error);
    EXPECT(message == "[anomaly:PCHBuildFail] stale build for main.cpp");

}  // namespace

ZEST_CASE(TrapInvokedPerReport) {
    /// The trap fires once per reported (non-suppressed) anomaly. In Debug
    /// builds the default trap aborts the process; the override used here is
    /// the mock point that lets us observe it in any build type.
    AnomalyCapture capture;

    LOG_ANOMALY(WorkerCrash, "worker {} died", 1);
    LOG_ANOMALY(WorkerCrash, "worker {} died", 2);

    ASSERT(capture.trapped.size() == 2u);
    EXPECT(capture.trapped[0] == AnomalyId::WorkerCrash);
}

ZEST_CASE(RateLimitSuppresses) {
    AnomalyCapture capture;

    for(std::uint32_t i = 0; i < logging::anomaly_report_limit + 5; ++i) {
        LOG_ANOMALY(CompileFail, "occurrence {}", i);
    }

    /// The client sees the reports plus one final suppression notice; the
    /// trap fires only for real reports.
    ASSERT(capture.notified.size() == logging::anomaly_report_limit + 1);
    EXPECT(capture.notified.back().second.find("report limit") != std::string::npos);
    EXPECT(capture.trapped.size() == logging::anomaly_report_limit);
}

ZEST_CASE(RateLimitPerId) {
    AnomalyCapture capture;

    for(std::uint32_t i = 0; i < logging::anomaly_report_limit + 5; ++i) {
        LOG_ANOMALY(CompileFail, "occurrence {}", i);
    }
    LOG_ANOMALY(PCMBuildFail, "different id still reports");

    /// CompileFail reports + its suppression notice + the PCMBuildFail report.
    EXPECT(capture.notified.size() == logging::anomaly_report_limit + 2);
}

ZEST_CASE(SuppressedArgsNotEvaluated) {
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

    EXPECT(evaluations == static_cast<int>(logging::anomaly_report_limit));
}

ZEST_CASE(LevelGateSkipsEvaluation) {
    AnomalyCapture capture;
    logging::options.level = logging::Level::off;

    int evaluations = 0;
    auto observe = [&evaluations] {
        ++evaluations;
        return 42;
    };
    LOG_ANOMALY(PCHBuildFail, "value {}", observe());

    EXPECT(evaluations == 0);
    EXPECT(capture.notified.size() == 0u);
    EXPECT(capture.trapped.size() == 0u);
}

ZEST_CASE(GuidanceMarkerAndLevel) {
    AnomalyCapture capture;

    LOG_GUIDANCE("no compilation database found in {}", "/tmp/ws");

    ASSERT(capture.notified.size() == 1u);
    auto& [level, message] = capture.notified.front();
    EXPECT(level == NotifyLevel::Warning);
    EXPECT(message == "[guidance] no compilation database found in /tmp/ws");
    EXPECT(capture.trapped.size() == 0u);
}

ZEST_CASE(GuidanceLazyAtLevel) {
    AnomalyCapture capture;
    logging::options.level = logging::Level::off;

    int evaluations = 0;
    auto observe = [&evaluations] {
        ++evaluations;
        return 1;
    };
    LOG_GUIDANCE("value {}", observe());

    EXPECT(evaluations == 0);
    EXPECT(capture.notified.size() == 0u);
}

ZEST_CASE(MarkerNamesStable) {
    /// Every id fires through the macro once and produces its wire marker.
    /// Integration tests grep these exact strings — keep them stable.
    AnomalyCapture capture;

    LOG_ANOMALY(PCHBuildFail, "x");
    LOG_ANOMALY(PCMBuildFail, "x");
    LOG_ANOMALY(CompileFail, "x");
    LOG_ANOMALY(WorkerRequestFail, "x");
    LOG_ANOMALY(WorkerCrash, "x");
    LOG_ANOMALY(WorkerSpawnFail, "x");
    LOG_ANOMALY(PositionMapFail, "x");
    LOG_ANOMALY(StaleTrust, "x");

    ASSERT(capture.notified.size() == logging::anomaly_id_count);
    const char* expected[] = {
        "PCHBuildFail",
        "PCMBuildFail",
        "CompileFail",
        "WorkerRequestFail",
        "WorkerCrash",
        "WorkerSpawnFail",
        "PositionMapFail",
        "StaleTrust",
    };
    for(std::size_t i = 0; i < logging::anomaly_id_count; ++i) {
        EXPECT(capture.notified[i].second == std::format("[anomaly:{}] x", expected[i]));
    }
}

};  // ZEST_SUITE(Anomaly)

}  // namespace
}  // namespace clice::testing
