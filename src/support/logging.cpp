module clice;

import :support.logging;
import :support.process;
import :support.version;

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

StderrReader reader = StderrReader::Terminal;

/// stderr: an editor's pipe, or for every other reader a descriptor
/// written blocking.
std::optional<PipeOutput> editor;
std::optional<FileOutput> console;
bool color = false;

/// The largest a log file grows before it moves aside; tests lower it.
std::uint64_t max_file_bytes = 0;

/// A file of the session's log directory: lines written before it opens
/// are kept, the latest 4 MiB of them, which it starts with (a server opens
/// its files with its first project); grown past max_file_bytes, it moves
/// to `<name>.1.log`, the one before going.
struct LogFile {
    std::optional<FileOutput> output;

    std::string path;

    std::deque<std::string> early;

    std::size_t early_bytes = 0;
};

/// The process's log, and the LSP messages the server traces
/// (lsp_trace_enabled).
LogFile file;
LogFile trace;

/// The local time's offset from UTC in seconds, as of the latest line: a
/// crash's line reads it, the handler unable to ask the time zone.
std::atomic<std::int64_t> utc_offset = 0;

/// The log file's descriptor, which a crash's trace goes to.
std::atomic<int> crash_fd = -1;

/// `time` on the local clock.
chrono::sys_time<chrono::milliseconds> local(chrono::system_clock::time_point time) {
    if(auto offset = sys::utc_offset(time)) {
        utc_offset.store(offset->count(), std::memory_order_relaxed);
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
    return !error                                        ? PipeSwitch::Done
           : error == std::errc::device_or_resource_busy ? PipeSwitch::Busy
                                                         : PipeSwitch::Refused;
}

/// Delivers the rest of a line whose head the pipe took, then the gap
/// report, staged when drops are unreported, then the backlog, as far as
/// the pipe takes them. The gap is older than any survivor, so its report
/// goes before them and lives outside the backlog: continued pressure can
/// evict buffered lines but never the count itself.
void pump(PipeOutput& output) {
    if(output.pipe == PipeSwitch::Busy) {
        output.pipe = switch_to_nonblocking(output.fd);
    }
    if(output.pipe != PipeSwitch::Done) {
        return;
    }
    // The first `count` bytes of the backlog; true when the pipe took them.
    auto deliver = [&](std::size_t count) {
        auto [written, _] = write_out(output.fd, std::string_view(output.pending).substr(0, count));
        if(written > 0) {
            output.front_partial = output.pending[written - 1] != '\n';
            output.pending.erase(0, written);
        }
        output.front_partial = output.front_partial && !output.pending.empty();
        return written == count;
    };
    if(output.front_partial && !deliver(output.pending.find('\n') + 1)) {
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
    deliver(output.pending.size());
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

/// Lines of `log` go to `fresh` from now on, under the descriptor number
/// it had: a crash's trace written meanwhile on another thread goes to one
/// file or the other, and the file before is closed, so that one rotated out
/// frees its space.
std::error_code log_to(LogFile& log, fs::File fresh) {
    auto size = fresh.status().transform([](const fs::Status& status) { return status.size; });
    if(log.output) {
        if(auto error = std::move(fresh).into_descriptor(log.output->fd)) {
            return error;
        }
        log.output->size = size.value_or(0);
        return {};
    }
    auto fd = std::move(fresh).into_descriptor();
    if(!fd) {
        return fd.error();
    }
    log.output = FileOutput{.fd = *fd, .size = size.value_or(0)};
    return {};
}

/// The file moves to `<name>.1.log`, the one before going, and a fresh
/// `<name>.log` goes on, saying so on its first line.
void rotate(LogFile& log) {
    auto older = log.path;
    fs::path::set_extension(older, ".1.log");
    log.output->size = 0;
    if(auto error = fs::rename(log.path, older)) {
        log.output->last_error = error;
        return;
    }
    auto fresh = fs::File::open(log.path, {.append = true, .create = true});
    if(auto error = fresh ? log_to(log, std::move(*fresh)) : fresh.error()) {
        log.output->last_error = error;
        fs::rename(older, log.path);
        return;
    }
    write(*log.output,
          std::format("[logging] the lines before are in {}\n", fs::path::filename(older)));
}

/// Write `line` to `log`, or keep it until the file opens.
void write(LogFile& log, std::string line) {
    if(!log.output) {
        log.early_bytes += line.size();
        log.early.push_back(std::move(line));
        while(log.early_bytes > (std::size_t{4} << 20)) {
            log.early_bytes -= log.early.front().size();
            log.early.pop_front();
        }
        return;
    }
    if(log.output->size + line.size() > max_file_bytes) {
        rotate(log);
    }
    write(*log.output, line);
}

/// Open `log` as `path`, the lines kept so far written first.
std::error_code open(LogFile& log, std::string path) {
    auto opened = fs::File::open(path, {.append = true, .create = true});
    if(auto error = opened ? log_to(log, std::move(*opened)) : opened.error()) {
        return error;
    }
    log.path = std::move(path);
    for(auto& line: log.early) {
        write(*log.output, line);
    }
    log.early.clear();
    log.early_bytes = 0;
    return {};
}

/// Where `path` is in `text` from `from` on: on Windows, which reads a path
/// with either separator and in either case, spelled either way.
std::size_t find_path(std::string_view text, std::string_view path, std::size_t from) {
    auto found =
        std::search(text.begin() + from, text.end(), path.begin(), path.end(), [](char a, char b) {
            if constexpr(fs::path::native_style == fs::path::Style::Windows) {
                return fs::path::is_separator(a) ? fs::path::is_separator(b)
                                                 : llvm::toLower(a) == llvm::toLower(b);
            }
            return a == b;
        });
    return found == text.end() ? std::string_view::npos : std::size_t(found - text.begin());
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
            for(auto from = find_path(rest, root.path, 0); from < at;
                from = find_path(rest, root.path, from + 1)) {
                // A whole path, not the tail of another (`/src` in `/usr/src/a.h`).
                auto end = from + root.path.size();
                bool starts = from == 0 || !(llvm::isAlnum(rest[from - 1]) ||
                                             llvm::StringRef("._-~+/\\").contains(rest[from - 1]));
                if(starts && end < rest.size() && fs::path::is_separator(rest[end])) {
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

/// The pid of the session a logging directory's entry is, or links to:
/// `<YYYY-MM-DD_HH-MM-SS>_<pid>` (start_session_log); none for another
/// entry.
std::optional<int> session_pid(llvm::StringRef name) {
    auto [started, pid] = name.rsplit('_');
    int value = 0;
    if(started.size() != std::string_view("YYYY-MM-DD_HH-MM-SS").size() ||
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
    std::vector<fs::Directory::Entry> sessions;
    for(auto& entry: *entries) {
        if(entry && session_pid(entry->name)) {
            sessions.push_back(std::move(*entry));
        }
    }
    // The names start with the start time: the latest sort first.
    ranges::sort(sessions, std::greater{}, &fs::Directory::Entry::name);
    for(auto [index, session]: views::enumerate(sessions)) {
        std::string path(logging_dir);
        fs::path::push(path, session.name);
        bool link = session.type == fs::FileType::Symlink;
        bool latest = static_cast<std::size_t>(index) + 1 < kept_sessions;
        bool kept = link ? latest && fs::is_directory(path)
                         : latest || sys::process_alive(*session_pid(session.name));
        if(kept) {
            continue;
        }
        // A link goes alone, never the session it links to.
        if(auto removed = fs::remove_all(path); !removed) {
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
    std::array<char, 512> header;
    auto end =
        std::format_to_n(header.data(),
                         header.size(),
                         "[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] [fatal] clice crashed\n"
                         "=== CRASH STACK TRACE ===\nclice {} {}{}{}{}\n",
                         static_cast<int>(date.year()),
                         static_cast<unsigned>(date.month()),
                         static_cast<unsigned>(date.day()),
                         clock.hours().count(),
                         clock.minutes().count(),
                         clock.seconds().count(),
                         clock.subseconds().count(),
                         version,
                         target,
                         commit.empty() ? "" : " ",
                         commit.empty() ? std::string_view() : source_tree,
                         commit)
            .out;
    sys::write_console(fd, std::string_view(header.data(), end));
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
    // Debug builds treat anomalies as assertion failures: the report is
    // written, then they abort. CLICE_ANOMALY_NO_TRAP exists for
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

PipeOutput pipe_output(PipeOutput output) {
    // A pipe that cannot be switched must never be written: a blocking
    // write to it is exactly the wedge this output exists to prevent.
    output.pipe = sys::is_pipe(output.fd) ? switch_to_nonblocking(output.fd) : PipeSwitch::Done;
    return output;
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
    if(output.unreported != 0 && all(std::format("{}[logging] dropped {} line(s): {}\n",
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

void stderr_logger(std::string_view name, StderrReader stderr_reader) {
    this_thread = 0;
    {
        std::lock_guard lock(mutex);
        process_name = name;
        reader = stderr_reader;
        if(reader != StderrReader::Editor) {
            console = FileOutput{.fd = 2};
            color = sys::shows_colors(2);
            return;
        }
        editor = pipe_output({.fd = 2});
        if(editor->pipe != PipeSwitch::Refused) {
            return;
        }
    }
    LOG_WARN("stderr is a pipe that cannot stop blocking clice: lines go to the log file alone");
}

void set_project_roots(llvm::ArrayRef<std::string> paths) {
    roots.clear();
    if(paths.size() > 1) {
        // Shown from the deepest directory above them all, sought from the
        // shortest's parent: a nested root's parent can be another root.
        // When it is the file system's root, or a drive's, they stay
        // absolute.
        auto& shortest = *ranges::min_element(paths, {}, &std::string::size);
        auto ancestors = fs::path::ancestors(fs::path::parent(shortest));
        auto common = ranges::find_if(ancestors, [&](std::string_view ancestor) {
            return ranges::all_of(paths,
                                  [&](auto& path) { return fs::path::is_within(path, ancestor); });
        });
        if(common == ancestors.end() || fs::path::filename(*common).empty()) {
            return;
        }
        for(auto& path: paths) {
            roots.push_back({
                .path = path,
                .shown = std::format("{}/", *fs::path::strip_prefix(path, *common)),
            });
        }
    } else if(!paths.empty()) {
        roots.push_back({.path = paths.front()});
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
        LOG_WARN("Cannot create the log directory {}: {}", logging_dir, made.error().message());
        return;
    }
    remove_old_sessions(logging_dir);
    std::string link(logging_dir);
    fs::path::push(link, fs::path::filename(session_dir));
    // A project opened again in the session is linked already.
    if(fs::is_directory(link)) {
        return;
    }
    if(auto error = fs::create_directory_symlink(session_dir, link)) {
        LOG_WARN("Cannot link {} to the session log {}: {}", link, session_dir, error.message());
    }
}

bool file_logger(std::string_view dir) {
    if(auto made = fs::create_directories(dir); !made) {
        LOG_ERROR("Cannot create the log directory {}: {}", dir, made.error().message());
        return false;
    }
    std::string path(dir);
    fs::path::push(path, std::format("{}.log", process_name));
    std::unique_lock lock(mutex);
    max_file_bytes = env_integer("CLICE_TEST_LOG_FILE_BYTES").value_or(std::uint64_t{64} << 20);
    if(auto error = open(file, path)) {
        lock.unlock();
        LOG_ERROR("Cannot open the log file {}: {}", path, error.message());
        return false;
    }
    crash_fd = file.output->fd;
    // Only the server traces, from its first message on.
    if(!trace.early.empty()) {
        std::string trace_path(dir);
        fs::path::push(trace_path, "trace.log");
        if(auto error = open(trace, trace_path)) {
            lock.unlock();
            LOG_ERROR("Cannot open the LSP trace {}: {}", trace_path, error.message());
        }
    }
    return true;
}

bool perf_lines() {
    const static bool wanted = !env_string("CLICE_PERF").value_or("").empty();
    return wanted;
}

bool lsp_trace_enabled() {
    const static bool wanted = !env_string("CLICE_LSP_TRACE").value_or("").empty();
    return wanted;
}

void trace_lsp(bool incoming, std::string_view message) {
    auto line = std::format("[{:%F %T}] {} {}\n",
                            local(chrono::system_clock::now()),
                            incoming ? "<-" : "->",
                            message);
    std::lock_guard lock(mutex);
    write(trace, std::move(line));
}

void log(Level level, std::source_location location, std::string text) {
    constexpr std::string_view names[] = {"trace", "debug", "info", "warning", "error", "fatal"};
    auto name = names[std::to_underlying(level)];
    text = relative_paths(std::move(text));
    auto line = std::format("[{:%F %T}] [{}] {}[{}:{}] {}\n",
                            local(chrono::system_clock::now()),
                            name,
                            this_thread == 0 ? "" : std::format("[thread {}] ", this_thread),
                            fs::path::filename(location.file_name()),
                            location.line(),
                            text);
    std::lock_guard lock(mutex);
    bool mirror = reader != StderrReader::Pool || !file.output;
    if(mirror && level >= options.stderr_level) {
        if(editor) {
            write(*editor, line);
        } else if(console) {
            auto paint = !color                  ? ""
                         : level >= Level::Error ? "\x1b[1;31m"
                         : level == Level::Warn  ? "\x1b[1;35m"
                                                 : "";
            write(*console,
                  std::format("{}{}:{} {}\n", paint, name, *paint ? "\x1b[0m" : "", text));
        }
    }
    write(file, std::move(line));
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
    if(reader != StderrReader::Pool) {
        // An editor's pipe may hold part of a line.
        sys::write_console(2, "\n");
        write_crash_header(2);
        sys::write_stacktrace(2, crash);
    }
}

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
