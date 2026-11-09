module clice:worker.crash_report;

import :support.prelude;

namespace clice {

/// Names the request the calling thread runs in the worker's last words.
/// A synchronous crash — a fault, an abort, a sanitizer report — is handled
/// on the thread that raised it, so a thread-local mark singles out the
/// request among those a stateful worker runs side by side, and a crash on
/// a thread without one names the request when it is the only one running;
/// the handler writes worker::crashed_in_marker and the tag to stderr, where
/// the master's drain picks it up.
class CrashScope {
public:
    explicit CrashScope(std::string tag);

    CrashScope(const CrashScope&) = delete;
    CrashScope& operator=(const CrashScope&) = delete;

    ~CrashScope();

private:
    std::string tag;
};

/// Write the crash line before the crash's trace (logging::write_crash),
/// and from the sanitizer's death callback. Call once, at worker startup,
/// on the thread that runs the event loop.
void install_crash_report();

}  // namespace clice
