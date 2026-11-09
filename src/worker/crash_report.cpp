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

/// The tags of the scopes open in the process, a slot each: a crash on a
/// thread no scope is open on, as the one clang continues a deep parse on
/// (clang::runWithSufficientStackSpace), names the request when it is the
/// only one running. Scopes open on libuv's pool threads and the compile
/// thread, far fewer than the slots.
std::array<std::atomic<const std::string*>, 64> open_tags = {};

/// Set as a crash is reported: a scope closing from then on waits for the
/// process to end, rather than free the tag the report may be reading.
std::atomic<bool> reporting = false;

/// The event loop's thread runs no request's work but between them, as
/// dropping a document's old AST or sending a reply: a crash there names
/// no request.
thread_local bool runs_event_loop = false;

/// The tag of the scope open alone, or null.
const std::string* only_open_tag() {
    const std::string* only = nullptr;
    for(auto& slot: open_tags) {
        if(auto* tag = slot.load()) {
            if(only) {
                return nullptr;
            }
            only = tag;
        }
    }
    return only;
}

void report_crash() {
    reporting.store(true);
    std::string_view tag(running_tag, running_size);
    if(!running_tag && !runs_event_loop) {
        if(auto* only = only_open_tag()) {
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
    [[maybe_unused]] bool slotted = false;
    for(auto& slot: open_tags) {
        const std::string* empty = nullptr;
        if(slot.compare_exchange_strong(empty, &this->tag)) {
            slotted = true;
            break;
        }
    }
    assert(slotted && "more scopes open than open_tags holds");

    run_test_hooks(this->tag);
}

CrashScope::~CrashScope() {
    for(auto& slot: open_tags) {
        const std::string* own = &tag;
        if(slot.compare_exchange_strong(own, nullptr)) {
            break;
        }
    }
    if(reporting.load()) {
        for(;;) {
            // No event: the end of the process, as the crash being reported
            // ends it.
            this_thread::sleep_for(chrono::hours(1));
        }
    }
    running_tag = nullptr;
    running_size = 0;
}

void install_crash_report() {
    runs_event_loop = true;
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
