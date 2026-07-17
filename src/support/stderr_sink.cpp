#include "support/stderr_sink.h"

#include <format>

#ifdef _WIN32
#include <io.h>

// See cache_store.cpp: windows.h must not spill min/max macros.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace clice::logging {

/// Whether the fd's drain is controlled by an external party. Pipes and
/// sockets qualify (editors, supervisors); terminals and files do not — a
/// tty/pty file description is shared with the parent shell, and neither
/// can exert client-controlled backpressure.
static bool externally_drained(int fd) {
#ifdef _WIN32
    HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    return handle != INVALID_HANDLE_VALUE && ::GetFileType(handle) == FILE_TYPE_PIPE;
#else
    struct stat st = {};
    return ::fstat(fd, &st) == 0 && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode));
#endif
}

/// Switch such an fd to non-blocking writes. False means the fd needs the
/// treatment but could not be switched — writing to it could still wedge
/// the caller, so the sink must not write at all.
static bool set_pipe_nonblocking(int fd) {
#ifdef _WIN32
    HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    return ::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr) != 0;
#else
    if(int flags = ::fcntl(fd, F_GETFL); flags >= 0) {
        return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }
    return false;
#endif
}

void restore_pipe_blocking(int fd) {
    if(!externally_drained(fd)) {
        return;
    }
#ifdef _WIN32
    HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
    ::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);
#else
    if(int flags = ::fcntl(fd, F_GETFL); flags >= 0) {
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
#endif
}

StderrSink::StderrSink(int fd, std::size_t capacity) : fd(fd), capacity(capacity) {
    // A pipe that cannot be switched must never be written: a blocking
    // write to it is exactly the wedge this sink exists to prevent.
    if(externally_drained(fd)) {
        disabled = !set_pipe_nonblocking(fd);
    }
}

std::size_t StderrSink::write_some(const char* data, std::size_t size) {
    std::size_t written = 0;
    while(written < size) {
#ifdef _WIN32
        // PIPE_NOWAIT: a full pipe reports success with zero (or partial)
        // bytes written instead of blocking.
        int n = ::_write(fd, data + written, static_cast<unsigned int>(size - written));
        if(n <= 0) {
            break;
        }
#else
        ssize_t n = ::write(fd, data + written, size - written);
        if(n <= 0) {
            if(n < 0 && errno == EINTR) {
                continue;
            }
            // EAGAIN: pipe full — the rest waits for a later delivery.
            // EPIPE and friends: reader gone; the backlog just ages out.
            break;
        }
#endif
        written += static_cast<std::size_t>(n);
    }
    return written;
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
        note_sent += write_some(active_note.data() + note_sent, active_note.size() - note_sent);
        if(note_sent < active_note.size()) {
            return false;
        }
        active_note.clear();
        note_sent = 0;
    }
    if(!pending.empty()) {
        auto n = write_some(pending.data(), pending.size());
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
    if(disabled) {
        dropped_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    stage_note_if_due();
    pump();

    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    if(active_note.empty() && pending.empty()) {
        auto n = write_some(formatted.data(), formatted.size());
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
    if(disabled) {
        return;
    }
    stage_note_if_due();
    pump();
}

}  // namespace clice::logging
