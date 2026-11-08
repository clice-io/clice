module clice;

import :support.log_sinks;

namespace clice::logging {

namespace {

/// How far one write of a buffer got: the bytes taken and, when it
/// stopped short, why.
struct Written {
    std::size_t bytes = 0;
    std::error_code error;
};

/// Writes until done or refused, `write_some` taking what one write takes.
/// A non-blocking pipe refuses when full; a blocking descriptor only on a
/// real error.
Written write_text(
    llvm::StringRef text,
    llvm::function_ref<std::expected<std::size_t, std::error_code>(std::string_view)> write_some) {
    Written result;
    while(result.bytes < text.size()) {
        auto written = write_some(text.drop_front(result.bytes));
        if(!written) {
            result.error = written.error();
            break;
        }
        result.bytes += *written;
    }
    return result;
}

Written write_fd(int fd, llvm::StringRef text) {
    return write_text(text,
                      [fd](std::string_view rest) { return sys::write_console_some(fd, rest); });
}

PipeSwitch set_pipe_nonblocking(int fd) {
    auto error = sys::set_nonblocking(fd);
    if(!error) {
        return PipeSwitch::Done;
    }
    return error == std::errc::device_or_resource_busy ? PipeSwitch::Busy : PipeSwitch::Refused;
}

}  // namespace

StderrSink::StderrSink(int fd, std::size_t capacity) : fd(fd), capacity(capacity) {
    // A pipe that cannot be switched must never be written: a blocking
    // write to it is exactly the wedge this sink exists to prevent.
    if(sys::is_pipe(fd)) {
        pipe = set_pipe_nonblocking(fd);
    }
}

bool StderrSink::retry_switch() {
    if(pipe == PipeSwitch::Busy) {
        pipe = set_pipe_nonblocking(fd);
    }
    return pipe == PipeSwitch::Done;
}

void StderrSink::stage_note_if_due() {
    // The gap is older than any survivor, so the report is delivered
    // ahead of everything and lives outside the backlog: continued
    // pressure can evict buffered lines but never the count itself.
    if(dropped_unreported > 0 && active_note.empty()) {
        active_note = std::format("[logging] dropped {} stderr line(s): client not draining\n",
                                  dropped_unreported);
        note_sent = 0;
        dropped_unreported = 0;
    }
}

bool StderrSink::pump() {
    if(!active_note.empty()) {
        note_sent += write_fd(fd, llvm::StringRef(active_note).drop_front(note_sent)).bytes;
        if(note_sent < active_note.size()) {
            return false;
        }
        active_note.clear();
        note_sent = 0;
    }
    if(!pending.empty()) {
        auto n = write_fd(fd, pending).bytes;
        if(n > 0) {
            front_partial = pending[n - 1] != '\n';
            pending.erase(0, n);
        }
        if(pending.empty()) {
            front_partial = false;
        }
        return pending.empty();
    }
    return true;
}

void StderrSink::shed_over_capacity() {
    if(pending.size() <= capacity) {
        return;
    }
    // Never cut the tail of a line whose head already reached the pipe;
    // the budget is soft by at most that one line.
    std::size_t start = 0;
    if(front_partial) {
        auto newline = pending.find('\n');
        if(newline == std::string::npos) {
            return;
        }
        start = newline + 1;
    }
    std::size_t cut = start;
    while(pending.size() - (cut - start) > capacity) {
        auto newline = pending.find('\n', cut);
        if(newline == std::string::npos) {
            break;
        }
        cut = newline + 1;
        dropped_total.fetch_add(1, std::memory_order_relaxed);
        dropped_unreported += 1;
    }
    pending.erase(start, cut - start);
}

void StderrSink::sink_it_(const spdlog::details::log_msg& msg) {
    bool writable = retry_switch();
    if(pipe == PipeSwitch::Refused) {
        dropped_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if(writable) {
        stage_note_if_due();
        pump();
    }

    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    if(writable && active_note.empty() && pending.empty()) {
        auto n = write_fd(fd, llvm::StringRef(formatted.data(), formatted.size())).bytes;
        if(n < formatted.size()) {
            pending.assign(formatted.data() + n, formatted.size() - n);
            front_partial = n > 0;
        }
    } else {
        // Older bytes go first: fresh content queues behind the backlog.
        pending.append(formatted.data(), formatted.size());
    }
    shed_over_capacity();
}

void StderrSink::flush_() {
    if(!retry_switch()) {
        return;
    }
    stage_note_if_due();
    pump();
}

FileSink::FileSink(int fd) : fd(fd) {}

FileSink::FileSink(fs::File file) : file(std::move(file)) {}

std::expected<std::shared_ptr<FileSink>, std::error_code> FileSink::open(llvm::StringRef path) {
    auto file = fs::File::open(path, {.append = true, .create = true});
    if(!file) {
        return std::unexpected(file.error());
    }
    return std::make_shared<FileSink>(std::move(*file));
}

bool FileSink::write_all(llvm::StringRef text) {
    auto written =
        file ? write_text(text, [&](std::string_view rest) { return file->write_some(rest); })
             : write_fd(fd, text);
    if(written.error) {
        last_error = written.error;
        torn = torn || written.bytes > 0;
        return false;
    }
    torn = false;
    return true;
}

bool FileSink::report_gap() {
    if(dropped_unreported == 0) {
        return true;
    }
    auto note = std::format("{}[logging] dropped {} line(s): {}\n",
                            torn ? "\n" : "",
                            dropped_unreported,
                            last_error.message());
    if(!write_all(note)) {
        return false;
    }
    dropped_unreported = 0;
    return true;
}

void FileSink::sink_it_(const spdlog::details::log_msg& msg) {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    if(report_gap() && write_all(llvm::StringRef(formatted.data(), formatted.size()))) {
        return;
    }
    dropped_total.fetch_add(1, std::memory_order_relaxed);
    dropped_unreported += 1;
}

}  // namespace clice::logging
