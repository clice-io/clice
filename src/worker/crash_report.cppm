module;

#include "modules/prelude.h"

module clice:worker.crash_report;

namespace clice {

/// Names the request the calling thread runs in the worker's last words.
/// A synchronous crash — a fault, an abort, a sanitizer report — is handled
/// on the thread that raised it, so a thread-local mark singles out the
/// culprit among the requests a stateful worker runs side by side; the
/// handler writes worker::crashed_in_marker and the tag to stderr, where
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

/// Hook the crash line into LLVM's signal handling and the sanitizer death
/// callback. Call once, at worker startup.
void install_crash_report();

}  // namespace clice
