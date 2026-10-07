module;

#include "modules/prelude.h"

module clice:support.log_sinks;

namespace clice::logging {

/// How switching an fd to non-blocking writes went.
enum class PipeSwitch : std::uint8_t {
    /// Switched, or the fd needs no switch.
    Done,
    /// Windows refuses while the pipe holds bytes its reader has not taken
    /// yet (ERROR_PIPE_BUSY); the switch can succeed once it has.
    Busy,
    /// The fd needs the treatment but cannot have it: writing to it could
    /// still wedge the caller, so every line is shed without touching it.
    Refused,
};

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
///
/// Windows refuses the switch while the pipe holds bytes the reader has
/// not taken yet, as at process start when something wrote to stderr
/// first (mimalloc writes a newline). Lines then wait in the buffer and
/// the switch is retried before each write, until the reader catches up.
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
        return pipe == PipeSwitch::Refused;
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;

    /// A flush is a delivery opportunity: push the note and backlog as far
    /// as the pipe allows, still without ever blocking.
    void flush_() override;

private:
    /// Materialize the gap report when drops are unreported and no report
    /// is already in flight.
    void stage_note_if_due();

    /// Deliver note remainder, then backlog, in place; true when drained.
    bool pump();

    /// Evict whole oldest lines until the backlog fits its budget.
    void shed_over_capacity();

    /// True once the fd may be written.
    bool retry_switch();

    int fd;
    PipeSwitch pipe = PipeSwitch::Done;
    std::size_t capacity;
    /// Gap report in flight, held OUTSIDE the backlog: eviction can never
    /// lose the count, and it is delivered ahead of everything — the gap
    /// is older than any survivor. note_sent tracks partial delivery.
    std::string active_note;
    std::size_t note_sent = 0;
    /// Bytes the pipe refused, flushed ahead of fresh content.
    std::string pending;
    /// The backlog's front continues a line whose head already reached
    /// the pipe: eviction must not cut it, or the torn line the buffer
    /// exists to prevent reappears.
    bool front_partial = false;
    std::atomic<std::size_t> dropped_total = 0;
    std::size_t dropped_unreported = 0;
};

/// A sink that writes each line straight through to a file descriptor —
/// the session log file, or stderr in every process but the serving
/// master (whose stderr reader is an editor, see StderrSink). Writes
/// block, as any command-line tool's do, but never take the process down:
/// spdlog's own file and console sinks abort on a failed write (exceptions
/// are compiled out), and a full disk under the log directory or a stderr
/// reader that went away is no reason to die. A line that cannot be
/// written is dropped, and the count with the error is written ahead of
/// the first line that gets through again.
class FileSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    /// Writes to `fd`, closing it on destruction when `owned`.
    FileSink(int fd, bool owned);

    ~FileSink() override;

    /// A sink appending to `path`, or why the file cannot be opened.
    static std::expected<std::shared_ptr<FileSink>, std::error_code> open(llvm::StringRef path);

    /// Lines dropped so far because a write failed.
    std::size_t dropped() const {
        return dropped_total.load(std::memory_order_relaxed);
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;

    /// Every line is written through; nothing is buffered.
    void flush_() override {}

private:
    /// Writes all of `text`; false when a write fails, with the error kept
    /// for the gap report.
    bool write_all(llvm::StringRef text);

    /// Writes the pending gap report, if any; false when it failed too.
    bool report_gap();

    int fd;
    bool owned;
    std::atomic<std::size_t> dropped_total = 0;
    std::size_t dropped_unreported = 0;
    std::error_code last_error;
    /// A failed write left part of a line behind and nothing followed it
    /// yet: the gap report starts on a line of its own.
    bool torn = false;
};

}  // namespace clice::logging
