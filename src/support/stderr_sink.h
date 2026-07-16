#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>

#include "spdlog/sinks/base_sink.h"

namespace clice::logging {

/// A stderr sink that drops instead of blocking.
///
/// fd 2's reader is the editor/client. Every client we support drains it,
/// but the server's liveness must not depend on that: once the 64KB pipe
/// fills, a plain write(2) parks the calling thread — the event loop —
/// until the client feels like reading, wedging the whole server. The
/// fallback is the simplest possible: when stderr is a pipe it is switched
/// to non-blocking, a full pipe costs the line instead of the caller, and
/// a summary line reports the gap once the client drains again. The file
/// log stays complete regardless — stderr is only a mirror.
///
/// Terminals and regular files are left in blocking mode: they cannot
/// exert client-controlled backpressure, and O_NONBLOCK on a tty is
/// shared with the parent shell's own file description.
class StderrSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit StderrSink(int fd = 2);

    /// Lines dropped so far because the pipe was full (observability).
    std::size_t dropped() const {
        return dropped_total.load(std::memory_order_relaxed);
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;

    void flush_() override {}

private:
    /// Write as much as the pipe accepts; false means it filled up (or the
    /// reader vanished) and the rest of the line was shed.
    bool try_write(const char* data, std::size_t size);

    int fd;
    std::atomic<std::size_t> dropped_total = 0;
    std::size_t dropped_unreported = 0;
};

}  // namespace clice::logging
