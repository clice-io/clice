module;

#include "version.h"

module clice;

import :support.logging;
import :support.process;

namespace clice::logging {

Options options;

namespace {

/// A directory paths under it are written relative to, with the text that
/// takes its place: empty for the one project root, the folder's name and
/// a separator for one of several.
struct Root {
    std::string path;
    std::string shown;
};

/// The project roots of the work the thread does, the longest first: a
/// nested project's paths are its own.
thread_local std::vector<Root> roots;

std::atomic<std::uint32_t> threads = 1;

/// The thread's number in its lines, 0 for the one stderr_logger ran on.
thread_local std::uint32_t this_thread = threads.fetch_add(1);

/// One line is written at a time, whatever thread logs it: lines of
/// several threads never mix, and the outputs below change under it.
std::mutex mutex;

std::string process_name;

/// stderr: an editor's pipe (Options::never_block_stderr), or for every
/// other process a terminal, a pipeline or the master's drain, its levels
/// coloured on a terminal.
std::optional<PipeOutput> editor;
std::optional<FileOutput> console;
bool color = false;

/// Every line goes to stderr too, at Options::stderr_level; not a worker's
/// once its log file opened.
bool mirror = true;

std::optional<FileOutput> file;
std::string file_path;

/// The largest a log file grows before it moves aside; tests lower it.
std::uint64_t max_file_bytes = 0;

/// The latest lines logged before the log file opened, which it starts with.
std::deque<std::string> early;

/// The local time's offset from UTC in seconds, as of the latest line: a
/// crash's line reads it, the handler unable to ask the time zone.
std::atomic<std::int64_t> utc_offset = 0;

/// The log file's descriptor, which a crash's trace goes to; one a
/// rotation replaced stays open, for a crash at that very moment.
std::atomic<int> crash_fd = -1;

/// `time` on the local clock.
chrono::sys_time<chrono::milliseconds> local(chrono::system_clock::time_point time) {
    auto seconds = chrono::floor<chrono::seconds>(time);
    if(auto tm = sys::local_time(seconds)) {
        auto day = chrono::sys_days(chrono::year(tm->tm_year + 1900) /
                                    chrono::month(static_cast<unsigned>(tm->tm_mon + 1)) /
                                    chrono::day(static_cast<unsigned>(tm->tm_mday)));
        auto as_utc = day + chrono::hours(tm->tm_hour) + chrono::minutes(tm->tm_min) +
                      chrono::seconds(tm->tm_sec);
        utc_offset.store((as_utc - seconds).count(), std::memory_order_relaxed);
    }
    return chrono::floor<chrono::milliseconds>(time) +
           chrono::seconds(utc_offset.load(std::memory_order_relaxed));
}

/// Writes `text` until done or refused: the bytes written, and why it
/// stopped short. A non-blocking pipe refuses when full; a blocking
/// descriptor only on a real error.
std::pair<std::size_t, std::error_code> write_out(int fd, std::string_view text) {
    std::size_t done = 0;
    while(done < text.size()) {
        auto written = sys::write_console_some(fd, text.substr(done));
        if(!written) {
            return {done, written.error()};
        }
        done += *written;
    }
    return {done, {}};
}

PipeSwitch switch_to_nonblocking(int fd) {
    auto error = sys::set_nonblocking(fd);
    return !error                                          ? PipeSwitch::Done
           : error == std::errc::device_or_resource_busy ? PipeSwitch::Busy
                                                         : PipeSwitch::Refused;
}

/// Delivers the gap report, staged when drops are unreported, then the
/// backlog, as far as the pipe takes them. The gap is older than any
/// survivor, so its report goes first and lives outside the backlog:
/// continued pressure can evict buffered lines but never the count itself.
void pump(PipeOutput& output) {
    if(output.pipe == PipeSwitch::Busy) {
        output.pipe = switch_to_nonblocking(output.fd);
    }
    if(output.pipe != PipeSwitch::Done) {
        return;
    }
    if(output.unreported > 0 && output.note.empty()) {
        output.note = std::format("[logging] dropped {} stderr line(s): client not draining\n",
                                  output.unreported);
        output.unreported = 0;
    }
    if(!output.note.empty()) {
        output.note_sent +=
            write_out(output.fd, std::string_view(output.note).substr(output.note_sent)).first;
        if(output.note_sent < output.note.size()) {
            return;
        }
        output.note.clear();
        output.note_sent = 0;
    }
    if(!output.pending.empty()) {
        auto [written, _] = write_out(output.fd, output.pending);
        if(written > 0) {
            output.front_partial = output.pending[written - 1] != '\n';
            output.pending.erase(0, written);
        }
        output.front_partial = output.front_partial && !output.pending.empty();
    }
}

/// Evict whole oldest lines until the backlog fits its budget. Never cut
/// the tail of a line whose head already reached the pipe; the budget is
/// soft by at most that one line.
void shed_over_capacity(PipeOutput& output) {
    auto& pending = output.pending;
    std::size_t start = 0;
    if(output.front_partial) {
        start = pending.find('\n');
        if(start == std::string::npos) {
            return;
        }
        start += 1;
    }
    std::size_t cut = start;
    while(pending.size() - (cut - start) > output.capacity) {
        auto newline = pending.find('\n', cut);
        if(newline == std::string::npos) {
            break;
        }
        cut = newline + 1;
        output.dropped += 1;
        output.unreported += 1;
    }
    pending.erase(start, cut - start);
}

/// Opens the log file at `path` to append to: its descriptor, and the bytes
/// it holds.
std::expected<FileOutput, std::error_code> open_log(llvm::StringRef path) {
    auto opened = fs::File::open(path, {.append = true, .create = true});
    if(!opened) {
        return std::unexpected(opened.error());
    }
    auto size = opened->status().transform([](const fs::Status& status) { return status.size; });
    return std::move(*opened).into_descriptor().transform([&](int fd) {
        return FileOutput{.fd = fd, .size = size.value_or(0)};
    });
}

/// The log file moves to `<name>.1.log`, the one before going, and a fresh
/// `<name>.log` goes on, saying so on its first line.
void rotate() {
    auto older = file_path;
    fs::path::set_extension(older, ".1.log");
    file->size = 0;
    if(auto error = fs::rename(file_path, older)) {
        file->last_error = error;
        return;
    }
    auto fresh = open_log(file_path);
    if(!fresh) {
        file->last_error = fresh.error();
        return;
    }
    file = std::move(*fresh);
    crash_fd = file->fd;
    write(*file, std::format("[logging] the lines before are in {}\n", fs::path::filename(older)));
}

/// `text`, every path under one of the thread's project roots relative to
/// it.
std::string relative_paths(std::string text) {
    if(roots.empty()) {
        return text;
    }
    std::string out;
    std::string_view rest = text;
    while(true) {
        std::size_t at = std::string_view::npos;
        const Root* found = nullptr;
        for(auto& root: roots) {
            for(auto from = rest.find(root.path); from < at; from = rest.find(root.path, from + 1)) {
                auto end = from + root.path.size();
                if(end < rest.size() && fs::path::is_separator(rest[end])) {
                    at = from;
                    found = &root;
                }
            }
        }
        if(!found) {
            out += rest;
            return out;
        }
        out += rest.substr(0, at);
        out += found->shown;
        rest.remove_prefix(at + found->path.size() + 1);
    }
}

/// Why `dir` could not be created: a file standing where one of its
/// directories goes is named, which Windows reports as a missing path.
std::string creation_error(llvm::StringRef dir, std::error_code error) {
    for(auto at: fs::path::ancestors(dir)) {
        if(fs::exists(at)) {
            return fs::is_directory(at) ? error.message() : std::format("{} is not a directory", at);
        }
    }
    return error.message();
}

/// The pid of the session a logging directory's entry is, or links to:
/// `<YYYY-MM-DD_HH-MM-SS>_<pid>` (start_session_log), `.txt` after it for a
/// note naming it; none for another entry.
std::optional<int> session_pid(llvm::StringRef name) {
    name.consume_back(".txt");
    auto [stamp, pid] = name.rsplit('_');
    int value = 0;
    if(stamp.size() != std::string_view("YYYY-MM-DD_HH-MM-SS").size() ||
       pid.getAsInteger(10, value)) {
        return std::nullopt;
    }
    return value;
}

/// How many sessions a logging directory keeps.
constexpr std::size_t kept_sessions = 20;

/// The sessions under `logging_dir` go but the latest kept_sessions - 1,
/// making room for one more, and those whose process still runs; so do
/// links to sessions gone.
void remove_old_sessions(std::string_view logging_dir) {
    auto entries = fs::Directory::open(logging_dir);
    if(!entries) {
        return;
    }
    llvm::SmallVector<std::string> sessions;
    for(auto& entry: *entries) {
        if(entry && session_pid(entry->name)) {
            sessions.push_back(std::move(entry->name));
        }
    }
    // The names start with the start time: the latest sort first.
    ranges::sort(sessions, std::greater{});
    for(auto [index, name]: views::enumerate(sessions)) {
        std::string path(logging_dir);
        fs::path::push(path, name);
        bool note = llvm::StringRef(name).ends_with(".txt");
        bool link = !note && fs::symlink_status(path)
                                 .transform([](const fs::Status& status) {
                                     return status.type == fs::FileType::Symlink;
                                 })
                                 .value_or(false);
        bool latest = static_cast<std::size_t>(index) + 1 < kept_sessions;
        bool kept = link ? latest && fs::is_directory(path)
                         : latest || (!note && sys::process_alive(*session_pid(name)));
        if(kept) {
            continue;
        }
        if(auto removed = link || note ? fs::remove(path)
                                       : fs::remove_all(path).transform([](auto) { return true; });
           !removed) {
            LOG_WARN("Cannot remove the old session log {}: {}", path, removed.error().message());
        }
    }
}

/// A crash's line in the log's form, then the build it names, so
/// symbolization uses the matching symbol file. Async-signal-safe: integers
/// and strings formatted into the stack, which reads no locale and
/// allocates nothing, the latest line's offset standing for the time zone.
void write_crash_header(int fd) noexcept {
    auto time = chrono::floor<chrono::milliseconds>(chrono::system_clock::now()) +
                chrono::seconds(utc_offset.load(std::memory_order_relaxed));
    auto day = chrono::floor<chrono::days>(time);
    chrono::year_month_day date(day);
    chrono::hh_mm_ss clock(time - day);
    std::array<char, 256> header;
    auto end = std::format_to_n(header.data(),
                                header.size(),
                                "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [fatal] clice crashed\n"
                                "=== CRASH STACK TRACE ===\nclice {} {}\n",
                                static_cast<int>(date.year()),
                                static_cast<unsigned>(date.month()),
                                static_cast<unsigned>(date.day()),
                                clock.hours().count(),
                                clock.minutes().count(),
                                clock.seconds().count(),
                                clock.subseconds().count(),
                                version,
                                target)
                   .out;
    sys::write_console(fd, std::string_view(header.data(), end));
}

}  // namespace

PipeOutput pipe_output(int fd, std::size_t capacity) {
    // A pipe that cannot be switched must never be written: a blocking
    // write to it is exactly the wedge this output exists to prevent.
    return {
        .fd = fd,
        .capacity = capacity,
        .pipe = sys::is_pipe(fd) ? switch_to_nonblocking(fd) : PipeSwitch::Done,
    };
}

void write(PipeOutput& output, std::string_view text) {
    pump(output);
    if(output.pipe == PipeSwitch::Refused) {
        output.dropped += 1;
        return;
    }
    // Older bytes go first: fresh content queues behind the backlog.
    if(output.pipe == PipeSwitch::Done && output.note.empty() && output.pending.empty()) {
        auto [written, _] = write_out(output.fd, text);
        text.remove_prefix(written);
        output.front_partial = written > 0 && !text.empty();
    }
    output.pending.append(text);
    shed_over_capacity(output);
    pump(output);
}

void write(FileOutput& output, std::string_view text) {
    auto all = [&](std::string_view bytes) {
        auto [written, error] = write_out(output.fd, bytes);
        output.size += written;
        output.torn = error ? output.torn || written > 0 : false;
        output.last_error = error ? error : output.last_error;
        return !error;
    };
    if(output.unreported != 0 &&
       all(std::format("{}[logging] dropped {} line(s): {}\n",
                       output.torn ? "\n" : "",
                       output.unreported,
                       output.last_error.message()))) {
        output.unreported = 0;
    }
    if(output.unreported == 0 && all(text)) {
        return;
    }
    output.dropped += 1;
    output.unreported += 1;
}

void stderr_logger(std::string_view name) {
    this_thread = 0;
    std::lock_guard lock(mutex);
    process_name = name;
    if(options.never_block_stderr) {
        editor = pipe_output(2);
        return;
    }
    console = FileOutput{.fd = 2};
#ifndef _WIN32
    // A Windows console shows escape sequences as they are unless asked to
    // read them.
    color = sys::is_console(2) && !env_string("NO_COLOR");
#endif
}

void set_project_roots(llvm::ArrayRef<std::string> paths) {
    roots.clear();
    if(paths.empty()) {
        return;
    }
    // The deepest directory holding them all, its separator kept.
    std::string_view common = paths.front();
    for(auto& path: paths.drop_front()) {
        common = common.substr(0, ranges::mismatch(common, path).in1 - common.begin());
    }
    if(paths.size() > 1) {
        common = common.substr(0, common.find_last_of("/\\") + 1);
        // The file system's root, or a drive's, holds every path.
        if(common.find_first_not_of("/\\") == std::string_view::npos || common.ends_with(":/") ||
           common.ends_with(":\\")) {
            return;
        }
    }
    for(auto& path: paths) {
        roots.push_back({
            .path = path,
            .shown = paths.size() > 1 ? path.substr(common.size()) + "/" : "",
        });
    }
    ranges::sort(roots, std::greater{}, [](const Root& root) { return root.path.size(); });
}

std::string start_session_log(std::string_view logging_dir) {
    remove_old_sessions(logging_dir);
    std::string directory(logging_dir);
    fs::path::push(directory,
                   std::format("{:%Y-%m-%d_%H-%M-%S}_{}",
                               chrono::floor<chrono::seconds>(local(chrono::system_clock::now())),
                               sys::pid()));
    return directory;
}

void link_session_log(std::string_view logging_dir, std::string_view session_dir) {
    if(auto made = fs::create_directories(logging_dir); !made) {
        LOG_WARN("Cannot create the log directory {}: {}",
                 logging_dir,
                 creation_error(logging_dir, made.error()));
        return;
    }
    remove_old_sessions(logging_dir);
    std::string link(logging_dir);
    fs::path::push(link, fs::path::filename(session_dir));
    if(!fs::create_directory_symlink(session_dir, link)) {
        return;
    }
    auto note = link + ".txt";
    if(auto error = fs::write_file(note, std::format("{}\n", session_dir))) {
        LOG_WARN("Cannot write {}: {}", note, error.message());
    }
}

bool file_logger(std::string_view dir, bool mirror_stderr) {
    if(auto made = fs::create_directories(dir); !made) {
        LOG_ERROR("Cannot create the log directory {}: {}", dir, creation_error(dir, made.error()));
        return false;
    }
    std::string path(dir);
    fs::path::push(path, std::format("{}.log", process_name));
    auto opened = open_log(path);
    if(!opened) {
        LOG_ERROR("Cannot open the log file {}: {}", path, opened.error().message());
        return false;
    }
    std::lock_guard lock(mutex);
    file = std::move(*opened);
    file_path = std::move(path);
    max_file_bytes = env_integer("CLICE_TEST_LOG_FILE_BYTES").value_or(std::uint64_t{64} << 20);
    crash_fd = file->fd;
    for(auto& line: early) {
        write(*file, line);
    }
    early.clear();
    mirror = mirror_stderr;
    return true;
}

void log(Level level, std::source_location location, std::string text) {
    constexpr std::string_view names[] = {"trace", "debug", "info", "warning", "error", "fatal"};
    auto name = names[std::to_underlying(level)];
    text = relative_paths(std::move(text));
    auto line = std::format("{:%F %T} [{}] {}{}:{} {}\n",
                            local(chrono::system_clock::now()),
                            name,
                            this_thread == 0 ? "" : std::format("[thread {}] ", this_thread),
                            fs::path::filename(location.file_name()),
                            location.line(),
                            text);
    std::lock_guard lock(mutex);
    if(mirror && level >= options.stderr_level) {
        if(editor) {
            write(*editor, line);
        } else if(console) {
            auto paint = !color                  ? ""
                         : level >= Level::Error ? "\x1b[1;31m"
                         : level == Level::Warn  ? "\x1b[1;35m"
                                                 : "";
            write(*console, std::format("{}{}:{} {}\n", paint, name, *paint ? "\x1b[0m" : "", text));
        }
    }
    if(!file) {
        if(early.size() == 128) {
            early.pop_front();
        }
        early.push_back(std::move(line));
        return;
    }
    if(file->size + line.size() > max_file_bytes) {
        rotate();
    }
    write(*file, line);
}

void fatal(std::source_location location, std::string text) {
    log(Level::Fatal, location, std::move(text));
    std::abort();
}

void handle_crashes() {
    local(chrono::system_clock::now());
    sys::on_crash(write_crash);
}

void write_crash(const sys::Crash& crash) noexcept {
    if(auto fd = crash_fd.load(); fd >= 0) {
        write_crash_header(fd);
        sys::write_stacktrace(fd, crash);
    }
    if(options.stderr_crash_trace) {
        write_crash_header(2);
        sys::write_stacktrace(2, crash);
    }
}

namespace {

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
        std::abort();
    }
#endif
}

}  // namespace

bool should_report(AnomalyId id) {
    if(options.level > Level::Error)
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
        log(Level::Error, std::source_location::current(), text);
        if(auto hook = current_notify_hook())
            hook(NotifyKind::Anomaly, text);
    }
    return false;
}

void report_anomaly(AnomalyId id, std::source_location location, std::string_view message) {
    auto text = std::format("[anomaly:{}] {}", id, message);
    log(Level::Error, location, text);
    if(auto hook = current_notify_hook())
        hook(NotifyKind::Anomaly, text);
    trap(id);
}

void report_guidance(std::source_location location, std::string_view message) {
    log(Level::Warn, location, std::string(message));
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
