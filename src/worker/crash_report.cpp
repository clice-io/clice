module;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

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

import :worker.crash_report;
import :worker.protocol;
import :worker.test_hooks;

namespace clice {

namespace {

/// Read from a signal handler: plain pointers into the owning scope's
/// string, never an object the handler would have to touch.
thread_local const char* running_tag = nullptr;
thread_local std::size_t running_size = 0;

void report_crash() {
    if(!running_tag) {
        return;
    }
    sys::write_console(2, worker::crashed_in_marker);
    sys::write_console(2, std::string_view(running_tag, running_size));
    sys::write_console(2, "\n");
}

}  // namespace

CrashScope::CrashScope(std::string tag) : tag(std::move(tag)) {
    running_tag = this->tag.data();
    running_size = this->tag.size();

    run_test_hooks(this->tag);
}

CrashScope::~CrashScope() {
    running_tag = nullptr;
    running_size = 0;
}

void install_crash_report() {
    llvm::sys::AddSignalHandler([](void*) { report_crash(); }, nullptr);
#ifdef _WIN32
    // The filter LLVM installs for the handler above also prints the
    // exception and a backtrace to stderr, which the master relays line by
    // line; a worker's trace belongs in its log file only, as on POSIX.
    // This filter keeps the rest of LLVM's: it runs the handlers and
    // removes the files registered for removal.
    ::SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG {
        llvm::sys::RunInterruptHandlers();
        return EXCEPTION_EXECUTE_HANDLER;
    });
#endif
#ifdef CLICE_ASAN
    // ASan reports memory errors and dies without raising a signal, and it
    // handles the faults it intercepts itself.
    __sanitizer_set_death_callback(report_crash);
#endif
}

}  // namespace clice
