#include "support/anomaly.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include "support/logging.h"

#include "llvm/Support/Process.h"

namespace clice::logging {

namespace {

std::array<std::atomic<std::uint32_t>, anomaly_id_count> report_counts;

/// Guards the two hooks below. Reporting sites copy the hook under the lock
/// and invoke it outside, so a hook may safely re-enter this module (e.g. a
/// testing trap calling reset_anomaly_for_testing).
std::mutex hook_mutex;

std::function<void(NotifyLevel, std::string_view)> notify_hook;

std::function<void(AnomalyId)> testing_trap;

std::function<void(NotifyLevel, std::string_view)> current_notify_hook() {
    std::lock_guard lock(hook_mutex);
    return notify_hook;
}

[[maybe_unused]] bool trap_disabled_by_env() {
    static bool disabled = llvm::sys::Process::GetEnv("CLICE_ANOMALY_NO_TRAP").has_value();
    return disabled;
}

void trap(AnomalyId id) {
    std::function<void(AnomalyId)> testing;
    {
        std::lock_guard lock(hook_mutex);
        testing = testing_trap;
    }
    if(testing) {
        testing(id);
        return;
    }
#ifndef NDEBUG
    // Debug builds treat anomalies as assertion failures: flush the log so
    // the report survives, then abort. CLICE_ANOMALY_NO_TRAP exists for
    // integration tests that intentionally trigger anomalies and verify the
    // Release behavior (report and continue).
    if(!trap_disabled_by_env()) {
        spdlog::shutdown();
        std::abort();
    }
#endif
}

}  // namespace

bool anomaly_should_report(AnomalyId id) {
    if(options.level > Level::err)
        return false;

    auto& count = report_counts[static_cast<std::size_t>(id)];
    auto previous = count.fetch_add(1, std::memory_order_relaxed);
    if(previous < anomaly_report_limit)
        return true;

    if(previous == anomaly_report_limit) {
        auto text =
            std::format("[anomaly:{}] report limit ({}) reached, suppressing further reports",
                        id,
                        anomaly_report_limit);
        logging::err("{}", text);
        if(auto hook = current_notify_hook())
            hook(NotifyLevel::Error, text);
    }
    return false;
}

void report_anomaly(AnomalyId id, std::string_view message, std::source_location location) {
    auto text = std::format("[anomaly:{}] {}", id, message);
    logging::log(spdlog::level::err, location, "{}", text);
    if(auto hook = current_notify_hook())
        hook(NotifyLevel::Error, text);
    trap(id);
}

bool guidance_should_report() {
    return options.level <= Level::warn;
}

void report_guidance(std::string_view message, std::source_location location) {
    auto text = std::format("[guidance] {}", message);
    logging::log(spdlog::level::warn, location, "{}", text);
    if(auto hook = current_notify_hook())
        hook(NotifyLevel::Warning, text);
}

void set_notify_hook(std::function<void(NotifyLevel, std::string_view)> hook) {
    std::lock_guard lock(hook_mutex);
    notify_hook = std::move(hook);
}

void set_anomaly_trap_for_testing(std::function<void(AnomalyId)> hook) {
    std::lock_guard lock(hook_mutex);
    testing_trap = std::move(hook);
}

void reset_anomaly_for_testing() {
    for(auto& count: report_counts)
        count.store(0, std::memory_order_relaxed);
    std::lock_guard lock(hook_mutex);
    testing_trap = nullptr;
}

}  // namespace clice::logging
