module;

#include "modules/prelude.h"

#ifdef _WIN32
#include <io.h>

// See cache_store.cpp: windows.h must not spill min/max macros.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

/// Writes until done or refused. A non-blocking pipe refuses when full
/// (EAGAIN; under Windows' PIPE_NOWAIT a write that took nothing); a
/// blocking descriptor only on a real error — or, for a Windows console,
/// at a Ctrl-Z, which `_write` treats as the end of the output.
Written write_fd(int fd, llvm::StringRef text) {
    Written result;
    while(result.bytes < text.size()) {
        auto rest = text.drop_front(result.bytes);
#ifdef _WIN32
        int n = ::_write(fd, rest.data(), static_cast<unsigned int>(rest.size()));
#else
        ssize_t n = ::write(fd, rest.data(), rest.size());
        if(n < 0 && errno == EINTR) {
            continue;
        }
#endif
        if(n <= 0) {
            result.error = n < 0 ? std::error_code(errno, std::generic_category())
                                 : std::make_error_code(std::errc::io_error);
            break;
        }
        result.bytes += static_cast<std::size_t>(n);
    }
    return result;
}

/// Whether the fd's drain is controlled by an external party. Pipes and
/// sockets qualify (editors, supervisors); terminals and files do not — a
/// tty/pty file description is shared with the parent shell, and neither
/// can exert client-controlled backpressure.
bool externally_drained(int fd) {
#ifdef _WIN32
    HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    return handle != INVALID_HANDLE_VALUE && ::GetFileType(handle) == FILE_TYPE_PIPE;
#else
    struct stat st = {};
    return ::fstat(fd, &st) == 0 && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode));
#endif
}

PipeSwitch set_pipe_nonblocking(int fd) {
#ifdef _WIN32
    HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if(::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) {
        return PipeSwitch::Done;
    }
    return ::GetLastError() == ERROR_PIPE_BUSY ? PipeSwitch::Busy : PipeSwitch::Refused;
#else
    if(int flags = ::fcntl(fd, F_GETFL);
       flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
        return PipeSwitch::Done;
    }
    return PipeSwitch::Refused;
#endif
}

}  // namespace

StderrSink::StderrSink(int fd, std::size_t capacity) : fd(fd), capacity(capacity) {
    // A pipe that cannot be switched must never be written: a blocking
    // write to it is exactly the wedge this sink exists to prevent.
    if(externally_drained(fd)) {
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

FileSink::FileSink(int fd, bool owned) : fd(fd), owned(owned) {}

FileSink::~FileSink() {
    if(owned) {
        llvm::sys::Process::SafelyCloseFileDescriptor(fd);
    }
}

std::expected<std::shared_ptr<FileSink>, std::error_code> FileSink::open(llvm::StringRef path) {
    int fd = -1;
    if(auto error = llvm::sys::fs::openFileForWrite(path,
                                                    fd,
                                                    llvm::sys::fs::CD_OpenAlways,
                                                    llvm::sys::fs::OF_Append)) {
        return std::unexpected(error);
    }
    return std::make_shared<FileSink>(fd, /*owned=*/true);
}

bool FileSink::write_all(llvm::StringRef text) {
    auto written = write_fd(fd, text);
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
