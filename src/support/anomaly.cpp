#include "support/anomaly.h"

#include <array>
#include <atomic>
#include <cstdlib>

#include "support/logging.h"

#include "llvm/Support/Process.h"

namespace clice::logging {

namespace {

std::array<std::atomic<std::uint32_t>, anomaly_id_count> report_counts;

std::function<void(NotifyLevel, std::string_view)> notify_hook;

std::function<void(AnomalyId)> testing_trap;

[[maybe_unused]] bool trap_disabled_by_env() {
    static bool disabled = llvm::sys::Process::GetEnv("CLICE_ANOMALY_NO_TRAP").has_value();
    return disabled;
}

void trap(AnomalyId id) {
    if(testing_trap) {
        testing_trap(id);
        return;
    }
#ifndef NDEBUG
    /// Debug builds treat anomalies as assertion failures: flush the log so
    /// the report survives, then abort. CLICE_ANOMALY_NO_TRAP exists for
    /// integration tests that intentionally trigger anomalies and verify the
    /// Release behavior (report and continue).
    if(!trap_disabled_by_env()) {
        spdlog::shutdown();
        std::abort();
    }
#endif
}

}  // namespace

std::string_view anomaly_name(AnomalyId id) {
    switch(id) {
        case AnomalyId::PchBuildFail: return "pch_build_fail";
        case AnomalyId::PcmBuildFail: return "pcm_build_fail";
        case AnomalyId::CompileFail: return "compile_fail";
        case AnomalyId::WorkerRequestFail: return "worker_request_fail";
        case AnomalyId::WorkerCrash: return "worker_crash";
        case AnomalyId::WorkerSpawnFail: return "worker_spawn_fail";
        case AnomalyId::PositionMapFail: return "position_map_fail";
    }
    return "unknown";
}

bool anomaly_should_report(AnomalyId id) {
    if(options.level > Level::err)
        return false;

    auto& count = report_counts[static_cast<std::size_t>(id)];
    auto previous = count.fetch_add(1, std::memory_order_relaxed);
    if(previous < anomaly_report_limit)
        return true;

    if(previous == anomaly_report_limit) {
        logging::err("[anomaly:{}] report limit ({}) reached, suppressing further reports",
                     anomaly_name(id),
                     anomaly_report_limit);
    }
    return false;
}

void report_anomaly(AnomalyId id, std::string_view message, std::source_location location) {
    auto text = std::format("[anomaly:{}] {}", anomaly_name(id), message);
    logging::log(spdlog::level::err, location, "{}", text);
    if(notify_hook)
        notify_hook(NotifyLevel::Error, text);
    trap(id);
}

bool guidance_should_report() {
    return options.level <= Level::warn;
}

void report_guidance(std::string_view message, std::source_location location) {
    auto text = std::format("[guidance] {}", message);
    logging::log(spdlog::level::warn, location, "{}", text);
    if(notify_hook)
        notify_hook(NotifyLevel::Warning, text);
}

void set_notify_hook(std::function<void(NotifyLevel, std::string_view)> hook) {
    notify_hook = std::move(hook);
}

void set_anomaly_trap_for_testing(std::function<void(AnomalyId)> hook) {
    testing_trap = std::move(hook);
}

void reset_anomaly_for_testing() {
    for(auto& count: report_counts)
        count.store(0, std::memory_order_relaxed);
    testing_trap = nullptr;
}

}  // namespace clice::logging
