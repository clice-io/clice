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

/// Switch the fd to non-blocking writes when (and only when) it is a pipe:
/// that is the one shape whose drain an external party controls.
static void set_pipe_nonblocking(int fd) {
#ifdef _WIN32
    HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    if(handle == INVALID_HANDLE_VALUE || ::GetFileType(handle) != FILE_TYPE_PIPE) {
        return;
    }
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    ::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);
#else
    struct stat st = {};
    if(::fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode)) {
        return;
    }
    if(int flags = ::fcntl(fd, F_GETFL); flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif
}

StderrSink::StderrSink(int fd) : fd(fd) {
    set_pipe_nonblocking(fd);
}

bool StderrSink::try_write(const char* data, std::size_t size) {
    while(size > 0) {
#ifdef _WIN32
        // PIPE_NOWAIT: a full pipe reports success with zero (or partial)
        // bytes written instead of blocking.
        int n = ::_write(fd, data, static_cast<unsigned int>(size));
        if(n <= 0) {
            return false;
        }
#else
        ssize_t n = ::write(fd, data, size);
        if(n <= 0) {
            if(n < 0 && errno == EINTR) {
                continue;
            }
            // EAGAIN: pipe full. EPIPE and friends: reader gone. Either
            // way the line is shed, never awaited.
            return false;
        }
#endif
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

void StderrSink::sink_it_(const spdlog::details::log_msg& msg) {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);

    // Report earlier drops before the next line that fits, so the gap is
    // visible where it happened. If the note itself does not fit, the
    // pipe is still full and this line joins the count.
    if(dropped_unreported > 0) {
        auto note = std::format("[logging] dropped {} stderr line(s): client not draining\n",
                                dropped_unreported);
        if(!try_write(note.data(), note.size())) {
            dropped_unreported += 1;
            dropped_total.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        dropped_unreported = 0;
    }

    if(!try_write(formatted.data(), formatted.size())) {
        dropped_unreported += 1;
        dropped_total.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace clice::logging
