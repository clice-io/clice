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

void StderrSink::write_or_buffer(const char* data, std::size_t size) {
    while(size > 0) {
#ifdef _WIN32
        // PIPE_NOWAIT: a full pipe reports success with zero (or partial)
        // bytes written instead of blocking.
        int n = ::_write(fd, data, static_cast<unsigned int>(size));
        if(n <= 0) {
            break;
        }
#else
        ssize_t n = ::write(fd, data, size);
        if(n <= 0) {
            if(n < 0 && errno == EINTR) {
                continue;
            }
            // EAGAIN: pipe full — keep the rest for a later flush. EPIPE
            // and friends: reader gone; the buffer then just ages out.
            break;
        }
#endif
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    pending.append(data, size);
}

void StderrSink::shed_over_capacity() {
    while(pending.size() > capacity) {
        auto newline = pending.find('\n');
        if(newline == std::string::npos) {
            dropped_total.fetch_add(1, std::memory_order_relaxed);
            dropped_unreported += 1;
            pending.clear();
            return;
        }
        pending.erase(0, newline + 1);
        dropped_total.fetch_add(1, std::memory_order_relaxed);
        dropped_unreported += 1;
    }
}

void StderrSink::sink_it_(const spdlog::details::log_msg& msg) {
    if(disabled) {
        dropped_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Flush what the pipe refused earlier; order is preserved by never
    // writing fresh content while older bytes are still pending.
    if(!pending.empty()) {
        std::string backlog;
        backlog.swap(pending);
        write_or_buffer(backlog.data(), backlog.size());
    }

    // Report earlier evictions once the backlog cleared: the note lands
    // where writes resumed, right before the next fresh line.
    if(pending.empty() && dropped_unreported > 0) {
        auto note = std::format("[logging] dropped {} stderr line(s): client not draining\n",
                                dropped_unreported);
        dropped_unreported = 0;
        write_or_buffer(note.data(), note.size());
    }

    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    write_or_buffer(formatted.data(), formatted.size());
    shed_over_capacity();
}

}  // namespace clice::logging
