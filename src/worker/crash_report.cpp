module;

#include "modules/prelude.h"

#ifdef _WIN32
#include <io.h>
#else
#include <signal.h>
#include <unistd.h>
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

import :vfs.file_system;
import :worker.crash_report;
import :worker.protocol;

namespace clice {

namespace {

/// Read from a signal handler: plain pointers into the owning scope's
/// string, never an object the handler would have to touch.
thread_local const char* running_tag = nullptr;
thread_local std::size_t running_size = 0;

void write_stderr(const char* data, std::size_t size) {
    while(size > 0) {
#ifdef _WIN32
        auto written = _write(2, data, static_cast<unsigned>(size));
#else
        auto written = ::write(2, data, size);
#endif
        if(written <= 0) {
            return;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

void report_crash() {
    if(!running_tag) {
        return;
    }
    write_stderr(worker::crashed_in_marker.data(), worker::crashed_in_marker.size());
    write_stderr(running_tag, running_size);
    write_stderr("\n", 1);
}

}  // namespace

CrashScope::CrashScope(std::string tag) : tag(std::move(tag)) {
    running_tag = this->tag.data();
    running_size = this->tag.size();

    // Tests crash the request whose tag contains CLICE_TEST_CRASH_REQUEST
    // right here — any kind of request, attributed like a real crash.
    static auto crash_request = llvm::sys::Process::GetEnv("CLICE_TEST_CRASH_REQUEST");
    if(crash_request && llvm::StringRef(this->tag).contains(*crash_request)) {
        LLVM_BUILTIN_TRAP;
    }

    // Tests kill the worker running the first request whose tag contains
    // the text of the file CLICE_TEST_KILL_REQUEST names, as a kill from
    // outside would: unannounced, the death names no request. Taking the
    // file away makes it the first — one rename of it succeeds, and a
    // respawned worker finds none.
    static auto kill_request = llvm::sys::Process::GetEnv("CLICE_TEST_KILL_REQUEST");
    if(kill_request) {
        auto wanted = vfs::read(*kill_request);
        if(wanted && llvm::StringRef(this->tag).contains((*wanted)->getBuffer().trim()) &&
           !vfs::rename(*kill_request, *kill_request + ".taken")) {
#ifdef _WIN32
            std::_Exit(1);
#else
            ::raise(SIGKILL);
#endif
        }
    }
}

CrashScope::~CrashScope() {
    running_tag = nullptr;
    running_size = 0;
}

void install_crash_report() {
    llvm::sys::AddSignalHandler([](void*) { report_crash(); }, nullptr);
#ifdef CLICE_ASAN
    // ASan reports memory errors and dies without raising a signal, and it
    // handles the faults it intercepts itself.
    __sanitizer_set_death_callback(report_crash);
#endif
}

}  // namespace clice
