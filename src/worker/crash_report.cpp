module;

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CLICE_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define CLICE_ASAN 1
#endif

#ifdef CLICE_ASAN
#include <sanitizer/common_interface_defs.h>
#endif

module clice;

import :support.logging;
import :worker.crash_report;
import :worker.protocol;
import :worker.test_hooks;

namespace clice {

namespace {

/// Read from a signal handler: plain pointers into the owning scope's
/// string, never an object the handler would have to touch.
thread_local const char* running_tag = nullptr;
thread_local std::size_t running_size = 0;

/// The scopes open in the process, and the tag of the one opened last: a
/// crash on a thread no scope is open on, as the one clang continues a deep
/// parse on (clang::runWithSufficientStackSpace), names the request when it
/// is the only one running.
std::atomic<std::size_t> open_scopes = 0;
std::atomic<const std::string*> last_opened = nullptr;

void report_crash() {
    std::string_view tag(running_tag, running_size);
    if(!running_tag && open_scopes.load() == 1) {
        if(auto* only = last_opened.load()) {
            tag = *only;
        }
    }
    if(tag.empty()) {
        return;
    }
    sys::write_console(2, worker::crashed_in_marker);
    sys::write_console(2, tag);
    sys::write_console(2, "\n");
}

}  // namespace

CrashScope::CrashScope(std::string tag) : tag(std::move(tag)) {
    running_tag = this->tag.data();
    running_size = this->tag.size();
    open_scopes += 1;
    last_opened = &this->tag;

    run_test_hooks(this->tag);
}

CrashScope::~CrashScope() {
    const std::string* own = &tag;
    last_opened.compare_exchange_strong(own, nullptr);
    open_scopes -= 1;
    running_tag = nullptr;
    running_size = 0;
}

void install_crash_report() {
    [[maybe_unused]] auto error = sys::on_crash([](const sys::Crash& crash) noexcept {
        report_crash();
        logging::write_crash(crash);
    });
#ifdef CLICE_ASAN
    // ASan reports memory errors and dies without raising a signal, and it
    // handles the faults it intercepts itself.
    __sanitizer_set_death_callback(report_crash);
#endif
}

}  // namespace clice
