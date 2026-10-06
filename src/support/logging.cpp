#include "support/logging.h"

#include <array>
#include <cassert>
#include <chrono>
#include <ctime>
#include <format>
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
#include "support/log_sinks.h"
#include "vfs/path.h"

#include "spdlog/sinks/ringbuffer_sink.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Signals.h"

namespace clice::logging {

Options options;

static std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> ringbuffer_sink;

/// The stderr sink stderr_logger chose, mirrored again by file_logger.
static spdlog::sink_ptr console_sink;

constexpr static auto pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] [%s:%#] %v";

void stderr_logger(std::string_view name, const Options& options) {
    std::shared_ptr<StderrSink> client_stderr;
    if(options.never_block_stderr) {
        client_stderr = std::make_shared<StderrSink>();
        console_sink = client_stderr;
    } else {
        console_sink = std::make_shared<FileSink>(2, /*owned=*/false);
    }

    std::shared_ptr<spdlog::logger> logger;
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

    if(client_stderr && client_stderr->inoperative()) {
        // The sink fails closed (drops everything) rather than risk the
        // caller blocking on an unswitchable pipe; the line reaches the
        // file log through the replay buffer.
        LOG_WARN("stderr mirror disabled: pipe could not be switched to non-blocking");
    }
}

std::string session_log_directory(std::string_view logging_dir) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
    return path::join(logging_dir, std::format("{}_{}", stamp, llvm::sys::Process::getProcessId()));
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
    auto opened = FileSink::open(filepath);
    if(!opened) {
        spdlog::error("Failed to open log file {}: {}", filepath, opened.error().message());
        return false;
    }
    auto file_sink = std::move(*opened);

    auto replay_buffer = ringbuffer_sink;

    llvm::SmallVector<spdlog::sink_ptr, 2> sinks = {file_sink};
    if(mirror_stderr) {
        assert(console_sink && "stderr_logger chooses the sink file_logger mirrors to");
        sinks.push_back(console_sink);
    }
    auto logger = std::make_shared<spdlog::logger>(std::string(name), sinks.begin(), sinks.end());
    logger->set_level(options.level);
    logger->set_pattern(pattern);
    logger->flush_on(Level::trace);
    spdlog::set_default_logger(std::move(logger));

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

#if defined(_WIN32)
static void print_frame(llvm::raw_ostream& os, DWORD64 pc) {
    os << llvm::format("0x%016llX", static_cast<unsigned long long>(pc));
    HMODULE module = nullptr;
    std::array<char, MAX_PATH> path;
    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(pc),
                          &module) &&
       GetModuleFileNameA(module, path.data(), path.size()) != 0) {
        auto base = reinterpret_cast<DWORD64>(module);
        os << llvm::format(", %s(0x%016llX) + 0x%llX byte(s)\n",
                           path.data(),
                           static_cast<unsigned long long>(base),
                           static_cast<unsigned long long>(pc - base));
    } else {
        os << " <unknown module>\n";
    }
}

/// LLVM's walk (dbghelp's StackWalk64) stops on arm64 at the exception
/// dispatcher's return address, which the system DLL signed with pointer
/// authentication, and RtlCaptureStackBackTrace follows the frame pointer
/// chain, which ends there too; ntdll's unwinder follows the unwind data,
/// strips the signatures and goes through the dispatcher to the crashing
/// frames. The frames print as LLVM's do without a symbolizer, the format
/// scripts/symbolize.py reads.
static void print_stack_trace(llvm::raw_ostream& os) {
    CONTEXT context;
    RtlCaptureContext(&context);
#if defined(__aarch64__)
    auto& pc = context.Pc;
#else
    auto& pc = context.Rip;
#endif
    for(int depth = 0; depth < 256 && pc != 0; depth += 1) {
        print_frame(os, pc);
        DWORD64 image_base = 0;
        auto* function = RtlLookupFunctionEntry(pc, &image_base, nullptr);
        if(!function) {
            // A leaf function, which returns through the link register on
            // arm64 and through the address on top of the stack on x64.
#if defined(__aarch64__)
            pc = context.Lr;
#else
            pc = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
#endif
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                         image_base,
                         pc,
                         function,
                         &context,
                         &handler_data,
                         &establisher_frame,
                         nullptr);
    }
}
#endif

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
#if defined(_WIN32)
        print_stack_trace(*crash_log_stream);
#else
        llvm::sys::PrintStackTrace(*crash_log_stream);
#endif
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
