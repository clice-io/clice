#include "support/logging.h"

#include <array>
#include <chrono>
#include <memory>
#include <string>

#if defined(__linux__)
#include <link.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
// See cache_store.cpp: windows.h must not spill min/max macros.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "version.h"
#include "support/filesystem.h"
#include "support/stderr_sink.h"

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Signals.h"

namespace clice::logging {

Options options;

static std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> ringbuffer_sink;

constexpr static auto pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] [%s:%#] %v";

void stderr_logger(std::string_view name, const Options& options) {
    std::shared_ptr<spdlog::logger> logger;

    auto console_sink = std::make_shared<StderrSink>();
    if(options.replay_console) {
        ringbuffer_sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(128);
        std::array<spdlog::sink_ptr, 2> sinks = {console_sink, ringbuffer_sink};
        logger = std::make_shared<spdlog::logger>(std::string(name), sinks.begin(), sinks.end());
    } else {
        logger = std::make_shared<spdlog::logger>(std::string(name), console_sink);
    }

    logger->set_level(options.level);
    logger->set_pattern(pattern);
    logger->flush_on(Level::trace);
    spdlog::set_default_logger(std::move(logger));
}

bool file_logger(std::string_view name,
                 std::string_view dir,
                 const Options& options,
                 bool mirror_stderr) {
    if(auto ec = llvm::sys::fs::create_directories(dir)) {
        spdlog::error("Failed to create log directory {}: {}", std::string(dir), ec.message());
        return false;
    }
    auto filepath = path::join(dir, std::format("{}.log", name));
    // Verify we can write to the file before constructing the sink.
    // (spdlog would throw on failure, but exceptions are disabled in this project.)
    {
        std::error_code ec;
        llvm::raw_fd_ostream test(filepath, ec, llvm::sys::fs::OF_Append);
        if(ec) {
            spdlog::error("Failed to open log file {}: {}", filepath, ec.message());
            return false;
        }
    }
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filepath);

    auto replay_buffer = ringbuffer_sink;

    llvm::SmallVector<spdlog::sink_ptr, 2> sinks = {file_sink};
    std::shared_ptr<StderrSink> mirror;
    if(mirror_stderr) {
        mirror = std::make_shared<StderrSink>();
        sinks.push_back(mirror);
    }
    auto logger = std::make_shared<spdlog::logger>(std::string(name), sinks.begin(), sinks.end());
    logger->set_level(options.level);
    logger->set_pattern(pattern);
    logger->flush_on(Level::trace);
    spdlog::set_default_logger(std::move(logger));

    if(mirror && mirror->inoperative()) {
        // The mirror fails closed (drops everything) rather than risk the
        // caller blocking on an unswitchable pipe; say so where it can be
        // seen — the file log.
        LOG_WARN("stderr mirror disabled: pipe could not be switched to non-blocking");
    }

    // Replay buffered logs after swapping the default logger, so no messages
    // emitted between the snapshot and the swap are lost.
    if(options.replay_console && replay_buffer) {
        file_sink->set_level(options.level);
        file_sink->set_pattern(pattern);

        for(auto& log: replay_buffer->last_raw()) {
            file_sink->log(log);
        }

        ringbuffer_sink.reset();
    }

    install_crash_handler(filepath, /*stderr_trace=*/mirror_stderr);
    return true;
}

static std::unique_ptr<llvm::raw_fd_ostream> crash_log_stream;

// Captured at install time: querying the dynamic loader is not
// async-signal-safe, so the handler may only print the cached value.
static uintptr_t executable_base = 0;

uintptr_t main_executable_base() {
#if defined(__linux__)
    uintptr_t base = 0;
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) {
            // The first entry is always the main executable.
            *static_cast<uintptr_t*>(data) = info->dlpi_addr;
            return 1;
        },
        &base);
    return base;
#elif defined(__APPLE__)
    // The slide, not the header address: Mach-O executables have a nonzero
    // preferred vmaddr, so only pc - slide yields the on-file address
    // symbolizers expect — the same contract as the ELF load bias above.
    return static_cast<uintptr_t>(_dyld_get_image_vmaddr_slide(0));
#elif defined(_WIN32)
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
#else
    return 0;
#endif
}

static void crash_handler(void*) {
    if(crash_log_stream) {
        *crash_log_stream << "\n=== CRASH STACK TRACE ===\n";
        // Identifies the exact artifact so symbolization uses the matching
        // symbol file (all constexpr string_views: async-signal-safe).
        *crash_log_stream << "clice " << version << " " << target << "\n";
        // Release binaries are PIE and stripped: the frame addresses below are
        // ASLR-shifted, so offline symbolization against the shipped symbol
        // file needs this rebase value (see scripts/symbolize.py). Zero is a
        // legitimate value (a zero macOS slide) and must still be recorded.
        *crash_log_stream << "main executable base: 0x";
        crash_log_stream->write_hex(executable_base);
        *crash_log_stream << "\n";
        llvm::sys::PrintStackTrace(*crash_log_stream);
        crash_log_stream->flush();
    }
}

void install_crash_handler(std::string_view log_path, bool stderr_trace) {
    executable_base = main_executable_base();
    std::error_code ec;
    crash_log_stream =
        std::make_unique<llvm::raw_fd_ostream>(llvm::StringRef(log_path.data(), log_path.size()),
                                               ec,
                                               llvm::sys::fs::OF_Append);
    if(ec) {
        LOG_WARN("Failed to install crash handler for {}: {}", log_path, ec.message());
        crash_log_stream.reset();
        return;
    }
    llvm::sys::AddSignalHandler(crash_handler, nullptr);
    // Workers skip the stderr backtrace: their crash traces belong in their
    // own log file only, not relayed into the master log via the stderr pipe.
    // Crash-only exception to the never-blocked-by-the-client rule: LLVM's
    // raw_ostream spins on a full non-blocking pipe, so a crash trace to an
    // undrained stderr can stall the dying process. The same trace already
    // reached the log file above, unaffected by fd 2's state.
    if(stderr_trace) {
        llvm::sys::PrintStackTraceOnErrorSignal("clice");
    }
}

}  // namespace clice::logging
