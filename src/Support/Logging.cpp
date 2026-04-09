#include "Support/Logging.h"

#include "Support/FileSystem.h"

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "llvm/Support/raw_ostream.h"

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

void file_loggger(std::string_view name, std::string_view dir, const Options& options) {
    if(auto err = fs::create_directories(dir)) {
        spdlog::error("Failed to create log directory {}: {}", std::string(dir), err.message());
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto filename = std::format("{:%Y-%m-%d_%H-%M-%S}.log", now);
    auto filepath = path::join(dir, filename);

    {
        std::error_code err;
        llvm::raw_fd_ostream test(filepath, err, fs::OF_Append);
        if(err) {
            spdlog::error("Failed to open log file {}: {}", filepath, err.message());
            return;
        }
    }

    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filepath);
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>(options.color);
    std::array<spdlog::sink_ptr, 2> sinks = {file_sink, console_sink};
    auto logger = std::make_shared<spdlog::logger>(std::string(name), sinks.begin(), sinks.end());
    logger->set_level(options.level);
    logger->set_pattern(pattern);
    logger->flush_on(Level::trace);
    spdlog::set_default_logger(std::move(logger));

    if(options.replay_console && ringbuffer_sink) {
        file_sink->set_level(options.level);
        file_sink->set_pattern(pattern);

        for(auto& log: ringbuffer_sink->last_raw()) {
            file_sink->log(log);
        }

        ringbuffer_sink.reset();
    }
}

}  // namespace clice::logging
