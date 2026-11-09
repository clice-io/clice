module;

#include "version.h"

module clice;

import :support.log_sinks;
import :support.logging;
import :support.process;

namespace clice::logging {

Options options;

namespace {

std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> ringbuffer_sink;

/// The stderr sink stderr_logger chose, mirrored again by file_logger.
spdlog::sink_ptr console_sink;

constexpr auto pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] [%s:%#] %v";

void install_logger(std::string_view name, llvm::ArrayRef<spdlog::sink_ptr> sinks, Level level) {
    auto logger = std::make_shared<spdlog::logger>(std::string(name), sinks.begin(), sinks.end());
    logger->set_level(level);
    logger->set_pattern(pattern);
    logger->flush_on(Level::trace);
    spdlog::set_default_logger(std::move(logger));
}

/// The log file a crash's stack trace is appended to, as a descriptor,
/// and the lines written before the trace: the handler may only write, and
/// a crash while the process exits finds them still, as no destructor frees
/// them.
int crash_log = -1;
std::array<char, 256> crash_header;
std::size_t crash_header_size = 0;

void open_crash_log(std::string_view log_path) {
    auto fd =
        fs::File::open(log_path, {.append = true, .create = true}).and_then([](fs::File file) {
            return std::move(file).into_descriptor();
        });
    if(!fd) {
        LOG_WARN("Cannot open {} for crash traces: {}", log_path, fd.error().message());
        return;
    }
    crash_log = *fd;
}

/// Why `dir` could not be created: a file standing where one of its
/// directories goes is named, which Windows reports as a missing path.
std::string creation_error(llvm::StringRef dir, std::error_code error) {
    for(auto at: fs::path::ancestors(dir)) {
        if(fs::exists(at)) {
            if(!fs::is_directory(at)) {
                return std::format("{} is not a directory", at);
            }
            break;
        }
    }
    return error.message();
}

std::array<std::atomic<std::uint32_t>, anomaly_id_count> report_counts;

/// Guards the two hooks below. Reporting sites copy the hook under the lock
/// and invoke it outside, so a hook may safely re-enter this module (e.g. a
/// testing trap calling reset_anomaly_for_testing).
std::mutex hook_mutex;

std::function<void(NotifyKind, std::string_view)> notify_hook;

std::function<void(AnomalyId)> testing_trap;

std::function<void(NotifyKind, std::string_view)> current_notify_hook() {
    std::lock_guard lock(hook_mutex);
    return notify_hook;
}

void trap(AnomalyId id) {
    std::function<void(AnomalyId)> testing;
    {
        std::lock_guard lock(hook_mutex);
        testing = testing_trap;
    }
    if(testing) {
        testing(id);
        return;
    }
#ifndef NDEBUG
    // Debug builds treat anomalies as assertion failures: flush the log so
    // the report survives, then abort. CLICE_ANOMALY_NO_TRAP exists for
    // integration tests that intentionally trigger anomalies and verify the
    // Release behavior (report and continue).
    const static bool disabled = env_string("CLICE_ANOMALY_NO_TRAP").has_value();
    if(!disabled) {
        LOG_ERROR(
            "Aborting on the anomaly above (Debug build); "
            "set CLICE_ANOMALY_NO_TRAP=1 to report it and continue instead");
        spdlog::shutdown();
        std::abort();
    }
#endif
}

}  // namespace

void stderr_logger(std::string_view name) {
    std::shared_ptr<StderrSink> client_stderr;
    if(options.never_block_stderr) {
        client_stderr = std::make_shared<StderrSink>();
        console_sink = client_stderr;
    } else {
        console_sink = std::make_shared<FileSink>(2);
    }
    console_sink->set_level(options.stderr_level);

    ringbuffer_sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(128);
    install_logger(name, {console_sink, ringbuffer_sink}, options.level);

    if(client_stderr && client_stderr->inoperative()) {
        // The sink fails closed (drops everything) rather than risk the
        // caller blocking on an unswitchable pipe; the line reaches the
        // file log through the replay buffer.
        LOG_WARN("stderr mirror disabled: pipe could not be switched to non-blocking");
    }
}

std::string session_log_directory(std::string_view logging_dir) {
    auto local = sys::local_time(chrono::system_clock::now()).value_or(std::tm{});
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", &local);
    std::string directory(logging_dir);
    fs::path::push(directory, std::format("{}_{}", timestamp, sys::pid()));
    return directory;
}

bool file_logger(std::string_view name, std::string_view dir, bool mirror_stderr) {
    if(auto made = fs::create_directories(dir); !made) {
        spdlog::error("Failed to create log directory {}: {}",
                      std::string(dir),
                      creation_error(dir, made.error()));
        return false;
    }
    std::string filepath(dir);
    fs::path::push(filepath, std::format("{}.log", name));
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
    install_logger(name, sinks, options.level);

    // Replay buffered logs after swapping the default logger, so no messages
    // emitted between the snapshot and the swap are lost.
    if(replay_buffer) {
        file_sink->set_level(options.level);
        file_sink->set_pattern(pattern);

        for(auto& log: replay_buffer->last_raw()) {
            file_sink->log(log);
        }

        ringbuffer_sink.reset();
    }

    open_crash_log(filepath);
    return true;
}

void handle_crashes() {
    // Names the exact build, so symbolization uses the matching symbol file.
    auto header = std::format_to_n(crash_header.data(),
                                   crash_header.size(),
                                   "\n=== CRASH STACK TRACE ===\nclice {} {}\n",
                                   version,
                                   target);
    crash_header_size = std::min(static_cast<std::size_t>(header.size), crash_header.size());
    sys::on_crash(write_crash);
}

void write_crash(const sys::Crash& crash) noexcept {
    if(crash_log >= 0) {
        sys::write_console(crash_log, {crash_header.data(), crash_header_size});
        sys::write_stacktrace(crash_log, crash);
    }
    if(options.stderr_crash_trace) {
        sys::write_console(2, {crash_header.data(), crash_header_size});
        sys::write_stacktrace(2, crash);
    }
}

bool should_report(AnomalyId id) {
    if(options.level > Level::err)
        return false;

    auto& count = report_counts[static_cast<std::size_t>(id)];
    auto previous = count.fetch_add(1, std::memory_order_relaxed);
    if(previous < anomaly_report_limit)
        return true;

    if(previous == anomaly_report_limit) {
        auto text =
            std::format("[anomaly:{}] report limit ({}) reached, suppressing further reports",
                        id,
                        anomaly_report_limit);
        logging::log(Level::err, std::source_location::current(), "{}", text);
        if(auto hook = current_notify_hook())
            hook(NotifyKind::Anomaly, text);
    }
    return false;
}

void report_anomaly(AnomalyId id, std::source_location location, std::string_view message) {
    auto text = std::format("[anomaly:{}] {}", id, message);
    logging::log(Level::err, location, "{}", text);
    if(auto hook = current_notify_hook())
        hook(NotifyKind::Anomaly, text);
    trap(id);
}

void report_guidance(std::source_location location, std::string_view message) {
    logging::log(Level::warn, location, "[guidance] {}", message);
    if(auto hook = current_notify_hook())
        hook(NotifyKind::Guidance, message);
}

void set_notify_hook(std::function<void(NotifyKind, std::string_view)> hook) {
    std::lock_guard lock(hook_mutex);
    notify_hook = std::move(hook);
}

void set_anomaly_trap_for_testing(std::function<void(AnomalyId)> hook) {
    std::lock_guard lock(hook_mutex);
    testing_trap = std::move(hook);
}

void reset_anomaly_for_testing() {
    for(auto& count: report_counts)
        count.store(0, std::memory_order_relaxed);
    std::lock_guard lock(hook_mutex);
    testing_trap = nullptr;
}

}  // namespace clice::logging
