#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

#include "spdlog/sinks/base_sink.h"

namespace clice::logging {

/// A stderr sink that buffers, then drops — never blocks.
///
/// fd 2's reader is the editor/client. Every client we support drains it,
/// but the server's liveness must not depend on that: once the 64KB pipe
/// fills, a plain write(2) parks the calling thread — the event loop —
/// until the client feels like reading, wedging the whole server. So when
/// stderr is a pipe it is switched to non-blocking, and what the pipe
/// refuses goes into a bounded in-memory buffer flushed by later log
/// calls: a slow reader loses nothing, torn lines cannot happen. Only a
/// reader that stays stuck past the buffer budget costs lines — whole
/// oldest lines are evicted and a summary line reports the gap once
/// writes flow again. The file log stays complete regardless — stderr is
/// only a mirror.
///
/// Terminals and regular files are left in blocking mode: they cannot
/// exert client-controlled backpressure, and O_NONBLOCK on a tty is
/// shared with the parent shell's own file description.
class StderrSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    /// `capacity` bounds the backpressure buffer: enough to ride out a
    /// busy editor's pauses, small enough that a dead reader cannot turn
    /// the mirror into a leak. Tests shrink it to exercise eviction.
    explicit StderrSink(int fd = 2, std::size_t capacity = 256 * 1024);

    /// Lines dropped so far because the pipe was full (observability).
    std::size_t dropped() const {
        return dropped_total.load(std::memory_order_relaxed);
    }

    /// The fd needed the non-blocking switch but refused it: the sink
    /// sheds everything rather than risk blocking the caller.
    bool inoperative() const {
        return disabled;
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;

    void flush_() override {}

private:
    /// Write as much as the pipe accepts; what it refuses is appended to
    /// `pending` for later flushes.
    void write_or_buffer(const char* data, std::size_t size);

    /// Evict whole oldest lines until the buffer fits its budget.
    void shed_over_capacity();

    int fd;
    /// The fd needs non-blocking treatment but could not be switched:
    /// every line is shed without touching the fd.
    bool disabled = false;
    std::size_t capacity;
    /// Bytes the pipe refused, flushed ahead of every later write.
    std::string pending;
    std::atomic<std::size_t> dropped_total = 0;
    std::size_t dropped_unreported = 0;
};

/// Undo StderrSink's non-blocking switch on a pipe/socket fd. Workers call
/// this right after their startup logger: their stderr is reserved for
/// third-party crash output (assertion failures, sanitizer reports) whose
/// writers expect blocking semantics, and its reader is the master's
/// always-running drain — a trusted party, unlike a client.
void restore_pipe_blocking(int fd = 2);

}  // namespace clice::logging
