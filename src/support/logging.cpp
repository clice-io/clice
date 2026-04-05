#include "support/logging.h"

#include <array>
#include <chrono>
#include <memory>
#include <string>

#include "support/filesystem.h"

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "llvm/Support/Signals.h"

namespace clice::logging {

Options options;

static std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> ringbuffer_sink;

constexpr static auto pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] [%s:%#] %v";

void stderr_logger(std::string_view name, const Options& options) {
    std::shared_ptr<spdlog::logger> logger;

    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>(options.color);
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

void file_logger(std::string_view name, std::string_view dir, const Options& options) {
    if(auto ec = llvm::sys::fs::create_directories(dir)) {
        spdlog::error("Failed to create log directory {}: {}", std::string(dir), ec.message());
        return;
    }
    auto filepath = path::join(dir, std::format("{}.log", name));
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filepath);

    if(options.replay_console && ringbuffer_sink) {
        file_sink->set_level(options.level);
        file_sink->set_pattern(pattern);

        for(auto& log: ringbuffer_sink->last_raw()) {
            file_sink->log(log);
        }

        ringbuffer_sink.reset();
    }

    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>(options.color);
    std::array<spdlog::sink_ptr, 2> sinks = {file_sink, console_sink};
    auto logger = std::make_shared<spdlog::logger>(std::string(name), sinks.begin(), sinks.end());
    logger->set_level(options.level);
    logger->set_pattern(pattern);
    logger->flush_on(Level::trace);
    spdlog::set_default_logger(std::move(logger));

    install_crash_handler(filepath);
}

static std::unique_ptr<llvm::raw_fd_ostream> crash_log_stream;

static void crash_handler(void*) {
    if(crash_log_stream) {
        *crash_log_stream << "\n=== CRASH STACK TRACE ===\n";
        llvm::sys::PrintStackTrace(*crash_log_stream);
        crash_log_stream->flush();
    }
}

void install_crash_handler(std::string_view log_path) {
    std::error_code ec;
    crash_log_stream =
        std::make_unique<llvm::raw_fd_ostream>(llvm::StringRef(log_path.data(), log_path.size()),
                                               ec,
                                               llvm::sys::fs::OF_Append);
    if(ec) {
        crash_log_stream.reset();
        return;
    }
    llvm::sys::AddSignalHandler(crash_handler, nullptr);
    llvm::sys::PrintStackTraceOnErrorSignal("clice");
}

}  // namespace clice::logging
