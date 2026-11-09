module clice:support.logging;

import :support.prelude;

/// # Logging & error-feedback design
///
/// Every report in clice answers one question first: WHO must act on it,
/// and is it clice's fault? That decides the channel — not how severe the
/// message feels. The channels:
///
///   situation                                   → channel
///   ─────────────────────────────────────────────────────────────────────
///   a clice invariant broke (a bug in clice)    → LOG_ANOMALY
///   the user must fix their setup or config     → LOG_GUIDANCE
///     ... and it is tied to a specific file     →   + a published diagnostic
///   a request cannot be served                  → LSP error response
///   an operation failed but is expected in       → LOG_WARN / LOG_ERROR
///     operation (user code errors, crash
///     windows, cancellations)
///   a measurement for profiling                 → LOG_PERF
///   the narrative of what the server did        → LOG_INFO/DEBUG/TRACE
///
/// Two hard rules fall out of this:
/// - LOG_ANOMALY is a soft assertion. If a condition is reachable through
///   user input or normal operation, it is NOT an anomaly — downgrade it.
///   Debug builds abort on anomalies (see CLICE_ANOMALY_NO_TRAP); Release
///   logs "[anomaly:<id>]" and pushes window/logMessage.
/// - Failures the user can cause must never abort or spam anomalies:
///   classify them structurally (has_user_errors, dispatch_errc) and log
///   them as warn/error instead.
///
/// The macros are in support/macros.h. Each decides whether its line
/// is written before it evaluates its arguments: they must not carry side
/// effects the caller needs.
///
/// ## Levels — LOG_TRACE .. LOG_FATAL
/// - trace: extremely verbose, per-token / per-payload detail.
/// - debug: control-flow detail for chasing a specific bug (scheduler
///   state changes, generation checks, deferred rebuilds).
/// - info: one line per meaningful event — startup steps, per-file
///   compile/index results, command decision logs. This is the default
///   level; an info-level session log must be enough to reconstruct what
///   the server did and why.
/// - warning: something went wrong but was handled or is expected in operation
///   (user-code compile errors, worker-down windows, cancelled requests).
/// - error: an operation failed and its result is lost or wrong; the server
///   keeps running.
/// - fatal (LOG_FATAL): unrecoverable; logs and aborts.
///
/// ## Perf lines — LOG_PERF(topic, ...)
/// Greppable measurements for profiling on real codebases, rendered as
/// "[perf:<topic>] key=value ..." at info level, e.g.
/// `grep 'perf:index' master.log`. Topics in use: "index" (per-file and
/// phase timings), "cache" (hit/miss with reason), "startup" (phase
/// durations), "request" (per-request latency on the master: wait_ms =
/// time until a worker was ready, total_ms = end-to-end), "query"
/// (worker-side share of a request: acquire_ms = AST wait + strand lock,
/// compute_ms = the feature call itself), "build" (stateless-worker build
/// tasks: kind=pch|pcm|compile_unit with per-stage splits — compile_ms, then
/// preamble_index_ms/flush_ms/state_write_ms for a pch, flush_ms for a
/// pcm, index_ms/teardown_ms for a unit's compile),
/// "index_query" (master-side index lookups: kind=relations|search),
/// "index_detail" (inside one index run: op=build splits the semantics
/// table from projection, instantiations, finishing and the encoding of
/// the shards and the TU index, op=preamble the document links and the
/// pch.idx index). Use stable key=value pairs and
/// `_ms` suffixes for durations — scripts aggregate these lines
/// (tools/ecosystem/perf/logs.ts).
///
/// ## Process ownership
/// The master logs to <logging_dir>/<session>/master.log and mirrors to
/// stderr, which editors show in their output panel. The mirror is
/// best-effort: clients are expected to drain stderr (editors do), and
/// one that stops reading costs log lines, never liveness — once the
/// pipe fills, lines are dropped and the gap is reported when the client
/// drains again (see PipeOutput). Workers log ONLY to
/// their own <session>/<worker>.log (mirror_stderr = false): a worker's
/// stderr is reserved for unexpected third-party output — assertion
/// failures, sanitizer reports — which the pool relays line-by-line into the
/// master log. A crash's stack trace (handle_crashes, from LOG_FATAL and
/// Debug anomaly traps too) is appended to the process's log file once
/// file_logger opened it, and printed to stderr, but a worker's: its
/// trace goes only to its own log file, none without a log dir.
namespace clice::logging {

enum class Level : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off,
};

struct Options {
    /// Lines below it are written nowhere.
    Level level = Level::Info;

    /// Lines below it are written to the log file alone: a session log keeps
    /// what a terminal reader does not want to see.
    Level stderr_level = Level::Info;

    /// stderr's reader is an editor, which must not be able to block the
    /// server: lines it does not take are buffered, then dropped (see
    /// PipeOutput). Every other process writes stderr blocking, as any
    /// command-line tool does — its reader is a terminal, a pipeline, or
    /// the master's drain.
    bool never_block_stderr = false;

    /// A crash's trace is printed to stderr too. Not a worker's, whose
    /// stderr the pool relays into the master's log line by line.
    bool stderr_crash_trace = true;
};

extern Options options;

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

/// Lines written to stderr that an editor reads: buffered, then dropped —
/// never blocking.
///
/// fd 2's reader is the editor/client. Every client we support drains it,
/// but the server's liveness must not depend on that: once the 64KB pipe
/// fills, a plain write(2) parks the calling thread — the event loop —
/// until the client feels like reading, wedging the whole server. So when
/// stderr is a pipe it is switched to non-blocking, and what the pipe
/// refuses goes into a bounded in-memory buffer flushed by later writes: a
/// slow reader loses nothing, torn lines cannot happen. Only a reader that
/// stays stuck past the buffer budget costs lines — whole oldest lines are
/// evicted and a summary line reports the gap once writes flow again. The
/// file log stays complete regardless — stderr is only a mirror.
///
/// Terminals and regular files are left in blocking mode: they cannot
/// exert client-controlled backpressure, and O_NONBLOCK on a tty is
/// shared with the parent shell's own file description.
///
/// Windows refuses the switch while the pipe holds bytes the reader has
/// not taken yet, as at process start when something wrote to stderr
/// first (mimalloc writes a newline). Lines then wait in the buffer and
/// the switch is retried before each write, until the reader catches up.
struct PipeOutput {
    int fd = 2;

    /// Bounds the backpressure buffer: enough to ride out a busy editor's
    /// pauses, small enough that a dead reader cannot turn the mirror into
    /// a leak. Tests shrink it to exercise eviction.
    std::size_t capacity = 256 * 1024;

    PipeSwitch pipe = PipeSwitch::Done;

    /// Gap report in flight, held OUTSIDE the backlog: eviction can never
    /// lose the count, and it is delivered ahead of everything — the gap
    /// is older than any survivor. note_sent tracks partial delivery.
    std::string note;
    std::size_t note_sent = 0;

    /// Bytes the pipe refused, flushed ahead of fresh content.
    std::string pending;

    /// The backlog's front continues a line whose head already reached
    /// the pipe: eviction must not cut it, or the torn line the buffer
    /// exists to prevent reappears.
    bool front_partial = false;

    /// Lines dropped so far because the pipe was full, and of those the ones
    /// no gap report told yet.
    std::size_t dropped = 0;
    std::size_t unreported = 0;
};

/// A PipeOutput writing to `fd`, a pipe switched to non-blocking.
PipeOutput pipe_output(int fd, std::size_t capacity = 256 * 1024);

/// Writes `text`, then pushes the note and backlog as far as the pipe
/// allows.
void write(PipeOutput& output, std::string_view text);

/// Lines written straight through to a file descriptor — the session log
/// file, or stderr in every process but the serving master (whose stderr
/// reader is an editor, see PipeOutput). Writes block, as any command-line
/// tool's do, but never take the process down: a full disk under the log
/// directory or a stderr reader that went away is no reason to die. A line
/// that cannot be written is dropped, and the count with the error is
/// written ahead of the first line that gets through again.
struct FileOutput {
    int fd = -1;

    /// The bytes written to it.
    std::uint64_t size = 0;

    /// Lines dropped so far because a write failed, and of those the ones
    /// no gap report told yet, with the last error.
    std::size_t dropped = 0;
    std::size_t unreported = 0;
    std::error_code last_error;

    /// A failed write left part of a line behind and nothing followed it
    /// yet: the gap report starts on a line of its own.
    bool torn = false;
};

void write(FileOutput& output, std::string_view text);

/// Log the process's lines to stderr, under `name`, which its log file
/// takes (file_logger); lines no log file takes yet are kept for it. Called
/// from main, before any other thread starts.
void stderr_logger(std::string_view name);

/// The lines the calling thread logs write paths under these project roots
/// relative to them: to the one root, or for several to the deepest
/// directory holding them all, the folders' names leading the paths (none
/// when that is the file system's root). Each thread sets the roots of the
/// work it does.
void set_project_roots(llvm::ArrayRef<std::string> roots);

/// A new session's log directory under `logging_dir`, named by the local
/// start time and the pid, so a run is found by the clock on the wall.
/// The sessions there but the latest 20 go, unless their process still
/// runs.
std::string start_session_log(std::string_view logging_dir);

/// Make the session logging in `session_dir`, under another project's
/// logging directory, found under `logging_dir` too: a link to it, or where
/// the system makes none (Windows without the right), a `.txt` naming it.
void link_session_log(std::string_view logging_dir, std::string_view session_dir);

/// Log to <dir>/<name>.log, the name stderr_logger took, starting with the
/// lines kept since. With mirror_stderr, every line is also written to
/// stderr — the master uses this so editors can show its log; workers must
/// pass false (their stderr is reserved for crash output, relayed by the
/// pool). Returns false when the log directory or file cannot be set up.
bool file_logger(std::string_view dir, bool mirror_stderr = true);

/// Write a crash's stack trace where `options` says: from main, before any
/// other thread starts.
void handle_crashes();

/// What handle_crashes writes for a crash, for a handler that writes its
/// own lines first; async-signal-safe.
void write_crash(const sys::Crash& crash) noexcept;

/// The LOG_* macros' line.
void log(Level level, std::source_location location, std::string text);

/// LOG_FATAL's line, then abort.
[[noreturn]] void fatal(std::source_location location, std::string text);

/// Stable identifiers for anomalies — internal states that should be
/// unreachable when clice works correctly. Reaching one indicates a bug in
/// clice itself, NOT a problem with user input or user code: those are
/// reported as LSP errors, guidance messages or plain logs instead.
///
/// Debug builds abort on the first occurrence so the CI Debug matrix and
/// local development show the bug as early as possible. Release builds
/// log the occurrence with an `[anomaly:<id>]` marker (the enumerator name),
/// forward it to the notify hook (master process: `window/logMessage`) and
/// continue running.
enum class AnomalyId : std::uint8_t {
    /// PCH build failed without a user-code error to explain it.
    PCHBuildFail,
    /// PCM build failed without a user-code error to explain it.
    PCMBuildFail,
    /// AST compile request to a stateful worker failed at the IPC layer.
    CompileFail,
    /// Feature query/build forwarded to a worker failed at the IPC layer.
    WorkerRequestFail,
    /// A worker process died unexpectedly.
    WorkerCrash,
    /// Worker pool failed to spawn or respawn a worker process.
    WorkerSpawnFail,
    /// An internally-produced offset or range failed to map to a position.
    PositionMapFail,
    /// A freshness check answered from an earlier look at a file that the
    /// disk contradicts (reported only while shadow checks run).
    StaleTrust,
    /// What the index built, or wrote under its own writer lock, does not
    /// read back.
    IndexReadBack,

    /// Number of ids, not a reportable anomaly — keep last.
    Count,
};

constexpr inline std::size_t anomaly_id_count = static_cast<std::size_t>(AnomalyId::Count);

/// Per-ID report cap per process; further occurrences are suppressed.
constexpr inline std::uint32_t anomaly_report_limit = 8;

/// Whether LOG_ANOMALY reports `id`: the log level and the per-ID cap, which
/// this counts against. Logs a final suppression notice when the cap is
/// reached.
bool should_report(AnomalyId id);

/// Log "[anomaly:<name>] <message>" at error level, forward it to the notify
/// hook, then trap: Debug builds abort unless the
/// trap is overridden for tests or CLICE_ANOMALY_NO_TRAP is set in the
/// environment; Release builds continue.
void report_anomaly(AnomalyId id, std::source_location location, std::string_view message);

/// Log the message at warn level and forward it to the notify hook (master process:
/// `window/showMessage`). For user-actionable situations that are not clice bugs (missing CDB,
/// invalid configuration,
/// ...).
void report_guidance(std::source_location location, std::string_view message);

/// What a message forwarded to the notify hook reports.
enum class NotifyKind : std::uint8_t {
    /// A bug in clice, for its log.
    Anomaly,
    /// A situation the user must fix, for the user to see.
    Guidance,
};

/// Process-wide hook observing anomaly/guidance messages. The master's
/// composition root (MasterServer) owns it for the server's lifetime and
/// materializes messages for transport delivery; workers never set it.
/// Hook registration is internally synchronized, so reporting from any
/// thread is safe — but the hook itself is invoked from the reporting
/// thread, so what it DOES must be safe there (the master's hook touches
/// event-loop state and is only ever invoked from the event-loop thread).
void set_notify_hook(std::function<void(NotifyKind, std::string_view)> hook);

/// Replace the debug trap so unit tests can observe anomalies (in any build
/// type) without aborting the test process.
void set_anomaly_trap_for_testing(std::function<void(AnomalyId)> trap);

/// Clear rate-limit counters and the testing trap.
void reset_anomaly_for_testing();

}  // namespace clice::logging

namespace clice {

/// Wall-clock time since it was made, for perf log lines (LOG_PERF).
struct Timer {
    chrono::steady_clock::time_point start = chrono::steady_clock::now();

    long long ms() const {
        return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start)
            .count();
    }

    /// Fractional milliseconds for sub-millisecond stages (feature compute,
    /// IPC hops); perf lines keep the `_ms` suffix convention.
    double precise_ms() const {
        return chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - start)
                   .count() /
               1000.0;
    }
};

}  // namespace clice
