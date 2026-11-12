module clice:unittests.utils.anomaly_capture;

import :support.logging;
import :support.prelude;

namespace clice::testing {

/// What clice reports on its anomaly and guidance channels while the
/// capture lives. An anomaly is trapped here instead of aborting a Debug
/// build, and its rate limit starts over with each capture.
struct AnomalyCapture {
    AnomalyCapture() : saved_level(logging::options.level) {
        logging::reset_anomaly_for_testing();
        logging::set_notify_hook([this](logging::NotifyKind kind, std::string_view message) {
            notified.emplace_back(kind, std::string(message));
        });
        logging::set_anomaly_trap_for_testing(
            [this](logging::AnomalyId id) { trapped.push_back(id); });
    }

    AnomalyCapture(const AnomalyCapture&) = delete;

    AnomalyCapture& operator=(const AnomalyCapture&) = delete;

    ~AnomalyCapture() {
        logging::set_notify_hook(nullptr);
        logging::reset_anomaly_for_testing();
        logging::options.level = saved_level;
    }

    /// How many anomalies of `id` were reported.
    std::size_t count(logging::AnomalyId id) const {
        return ranges::count(trapped, id);
    }

    /// The guidance messages, in the order they were reported.
    std::vector<std::string> guidance() const {
        std::vector<std::string> messages;
        for(auto& [kind, message]: notified) {
            if(kind == logging::NotifyKind::Guidance) {
                messages.push_back(message);
            }
        }
        return messages;
    }

    std::vector<std::pair<logging::NotifyKind, std::string>> notified;

    std::vector<logging::AnomalyId> trapped;

    logging::Level saved_level;
};

}  // namespace clice::testing
