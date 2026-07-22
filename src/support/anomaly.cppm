export module clice:support.anomaly;

import stdlib;

/// Anomaly and guidance reporting — the policy layer on top of the logging
/// transport. The full channel design (which situation goes to which
/// channel, and why this is separate from the logging transport) is documented
/// at the top of support/logging.cppm. The LOG_ANOMALY / LOG_GUIDANCE macros
/// that drive these functions live in support/prelude.h.
export namespace clice::logging {

/// Stable identifiers for anomalies — internal states that should be
/// unreachable when clice works correctly. Reaching one indicates a bug in
/// clice itself, NOT a problem with user input or user code: those are
/// reported as LSP errors, guidance messages or plain logs instead.
///
/// Debug builds abort on the first occurrence so the CI Debug matrix and
/// local development surface the bug as early as possible. Release builds
/// log the occurrence with an `[anomaly:<id>]` marker (the enumerator name,
/// rendered via the reflective enum formatter), forward it to the
/// notify hook (master process: `window/logMessage`) and continue running.
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

    /// Number of ids, not a reportable anomaly — keep last.
    Count,
};

constexpr inline std::size_t anomaly_id_count = static_cast<std::size_t>(AnomalyId::Count);

/// Per-ID report cap per process; further occurrences are suppressed.
constexpr inline std::uint32_t anomaly_report_limit = 8;

/// Gate evaluated BEFORE the format arguments of LOG_ANOMALY: checks the log
/// level and bumps the per-ID rate-limit counter. Logs a final suppression
/// notice when the cap is reached.
bool anomaly_should_report(AnomalyId id);

/// Log "[anomaly:<name>] <message>" at err level, forward it to the notify
/// hook, then trap: Debug builds abort (after flushing the log) unless the
/// trap is overridden for tests or CLICE_ANOMALY_NO_TRAP is set in the
/// environment; Release builds continue.
void report_anomaly(AnomalyId id,
                    std::string_view message,
                    std::source_location location = std::source_location::current());

/// Gate evaluated BEFORE the format arguments of LOG_GUIDANCE.
bool guidance_should_report();

/// Log "[guidance] <message>" at warn level and forward it to the notify hook.
/// For user-actionable situations that are not clice bugs (missing CDB,
/// invalid configuration, ...).
void report_guidance(std::string_view message,
                     std::source_location location = std::source_location::current());

/// Severity forwarded to the notify hook; values mirror LSP MessageType.
enum class NotifyLevel : std::uint8_t {
    Error = 1,
    Warning = 2,
};

/// Process-wide hook observing anomaly/guidance messages. The master's
/// composition root (MasterServer) owns it for the server's lifetime and
/// materializes messages for transport delivery; workers never set it.
/// Hook registration is internally synchronized, so reporting from any
/// thread is safe — but the hook itself is invoked from the reporting
/// thread, so what it DOES must be safe there (the master's hook touches
/// event-loop state and is only ever invoked from the event-loop thread).
void set_notify_hook(std::function<void(NotifyLevel, std::string_view)> hook);

/// Replace the debug trap so unit tests can observe anomalies (in any build
/// type) without aborting the test process.
void set_anomaly_trap_for_testing(std::function<void(AnomalyId)> trap);

/// Clear rate-limit counters and the testing trap.
void reset_anomaly_for_testing();

}  // namespace clice::logging
