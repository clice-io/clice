#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compile/dep_file.h"
#include "config/config.h"
#include "feature/feature.h"
#include "syntax/token.h"

#include "kota/codec/json/json.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/protocol.h"

namespace clice::worker {

namespace protocol = kota::ipc::protocol;

/// Error codes attached to master-side dispatch failures. They mark expected
/// operational conditions — memory-pressure preemption and crash/restart
/// windows — as opposed to real IPC breakage: callers must not classify them
/// as anomalies (see support/anomaly.h). The crash itself is already reported
/// as a WorkerCrash anomaly by the pool.
namespace dispatch_errc {

/// The request was deliberately cancelled (memory-pressure preemption).
constexpr inline protocol::integer cancelled =
    static_cast<protocol::integer>(protocol::ErrorCode::RequestCancelled);

/// No live worker could take the request (crash/restart window or pool stop).
constexpr inline protocol::integer worker_unavailable = -33000;

/// The worker process died while serving the request. The pool does not
/// retry: it marks the slot dead and surfaces this code so the caller can
/// decide — stateless build tasks are idempotent and safe to resend, while
/// e.g. the indexer prefers to requeue the file instead.
constexpr inline protocol::integer worker_crashed = -33001;

/// The assigned worker is mid-restart after a crash: the request was never
/// dispatched. Distinct from worker_crashed so crash accounting (document
/// quarantine) does not blame a document for a window it merely hit.
constexpr inline protocol::integer worker_restarting = -33002;

}  // namespace dispatch_errc

/// True when a dispatch failure is an expected operational condition rather
/// than clice infrastructure breakage.
inline bool is_operational_error(const protocol::Error& error) {
    return error.code == dispatch_errc::cancelled ||
           error.code == dispatch_errc::worker_unavailable ||
           error.code == dispatch_errc::worker_crashed ||
           error.code == dispatch_errc::worker_restarting;
}

/// Identity of the worker incarnation a crashed request died with, carried
/// in Error::data. One process death fails every request in flight on it;
/// per-content blame (Quarantine) dedups by this identity so a single death
/// is counted at most once per document.
inline protocol::Value death_identity(std::size_t index, unsigned generation, bool stateful) {
    return std::format("{}:{}:{}", stateful ? "sf" : "sl", index, generation);
}

/// The death identity attached to a worker_crashed error; empty when the
/// error carries none (locally synthesized failures).
inline std::string_view death_of(const protocol::Error& error) {
    if(error.data.has_value()) {
        if(auto* id = std::get_if<std::string>(&*error.data)) {
            return *id;
        }
    }
    return {};
}

/// True for errors produced by the IPC transport itself (broken pipe, closed
/// peer) as opposed to errors returned by the remote handler. kota surfaces
/// transport failures with the default RequestFailed code; clice worker
/// handlers never return that code, so it identifies a dead worker link.
inline bool is_transport_error(const protocol::Error& error) {
    return error.code == static_cast<protocol::integer>(protocol::ErrorCode::RequestFailed);
}

/// Kind of AST query dispatched to a stateful worker.
enum class QueryKind : uint8_t {
    Hover,
    GoToDefinition,
    SemanticTokens,
    InlayHints,
    FoldingRange,
    DocumentSymbol,
    CodeAction,
};

/// Unified parameters for all stateful AST queries.
/// The worker dispatches to the appropriate feature handler based on `kind`.
struct QueryParams {
    QueryKind kind;
    std::string path;
    uint32_t offset = 0;  ///< Byte offset for position-sensitive queries (Hover, GoToDefinition).
    LocalSourceRange range;  ///< Byte range for range-sensitive queries (InlayHints).

    /// The workspace config, carried whole on every request — the worker
    /// holds no config state and a config change simply shows up on the
    /// next request. Features read their own section; no per-feature
    /// forwarding field is ever added here.
    Config config;
};

/// Parameters for stateful compilation (builds AST, publishes diagnostics).
struct CompileParams {
    std::string path;
    int version;
    std::string text;
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;

    /// Conditional levels left open by the PCH's preamble (1 = branch
    /// inactive), seeding the main compile's inactive-region scan.
    std::vector<std::uint8_t> open_conditionals;
};

/// Outcome of a stateful compile. Anything but `Done` is a non-result: the
/// master must not settle the session, record dependencies, or publish the
/// (empty) diagnostics as current — doing so freezes the document on a
/// product that never existed.
enum class CompileStatus : uint8_t {
    /// The parse produced a usable product — a complete AST, or a fatal
    /// error whose diagnostics describe the user's code.
    Done,
    /// The parse was interrupted by CancelCompile (superseded round).
    Cancelled,
    /// The frontend failed before parsing began: bad invocation, or a
    /// prebuilt input (PCH/PCM) clang could not read. Whether the consumed
    /// PCH is to blame is a separate signal (pch_suspect).
    SetupFail,
};

struct CompileResult {
    CompileStatus status = CompileStatus::Done;

    /// The parse failed and its diagnostics blame the consumed PCH — they
    /// name the blob's path, or they are AST-deserialization errors naming
    /// no other prebuilt input. Holds whether the reader rejected the blob
    /// at setup or hit a fatal error past it (a Done status with real,
    /// publishable diagnostics). Either way the artifact is the culprit:
    /// the master should retract the pair and rebuild instead of trusting
    /// the corrupt bytes until the preamble changes.
    bool pch_suspect = false;

    int version;
    /// Diagnostics serialized as JSON (RawValue) to avoid bincode/serde annotation conflicts.
    kota::codec::RawValue diagnostics;
    std::size_t memory_usage;
    /// Milliseconds since epoch, sampled before the compile started. Files
    /// whose mtime is past this moment may differ from what the build read.
    std::int64_t build_at = 0;
    std::vector<DepFile> deps;
    /// Serialized TUIndex for the main file (interested_only=true).
    std::string tu_index_data;

    /// Preprocessor-inactive regions as flat byte-offset pairs
    /// [begin0, end0, begin1, end1, ...] in the main file. Covers only
    /// the content past the PCH bound; the preamble's share lives in
    /// PCHState (analogous to document links).
    std::vector<std::uint32_t> inactive_regions;
};

/// Build a PCH (and its paired pch.idx envelope) from preamble content.
struct BuildPCHParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;

    /// The preamble content, remapped over the file.
    std::string content;
    uint32_t preamble_bound = UINT32_MAX;

    /// Tmp path allocated by the master's store; the master commits
    /// (fsync + atomic rename) after the worker reports success.
    std::string output_path;

    /// Tmp path for the pch.idx envelope, allocated alongside output_path.
    /// The worker serializes the preamble's index and feature state into
    /// it; the master commits both blobs together.
    std::string index_output_path;
};

/// Build a module interface's PCM.
struct BuildPCMParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;

    std::string module_name;

    /// Transitive PCM dependencies (module name -> artifact path).
    std::unordered_map<std::string, std::string> pcms;

    /// Tmp path allocated by the master's store (see BuildPCHParams).
    std::string output_path;
};

/// One whole-TU run: a single parse serving the products the frozen plan
/// names — the full index, a clang-tidy pass, or both.
struct TURunParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;

    /// PCM dependencies for TUs that import modules.
    std::unordered_map<std::string, std::string> pcms;

    /// Products of the run.
    bool index = false;
    bool tidy = false;

    /// Frozen clang-tidy configuration (see tidy::TidyParams); meaningful
    /// only when `tidy` is set.
    std::string tidy_checks;
    std::vector<std::pair<std::string, std::string>> tidy_options;
    std::string tidy_warnings_as_errors;
    std::string tidy_header_filter;
    std::string tidy_exclude_header_filter;
    bool tidy_system_headers = false;
    std::vector<std::string> tidy_extra_args;
    std::vector<std::string> tidy_extra_args_before;
};

/// Code completion over unsaved buffer content.
struct CompletionParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;

    std::string text;
    uint32_t offset = 0;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;

    /// The workspace config, carried whole — the worker holds no config
    /// state and a config change simply shows up on the next request.
    Config config;
};

/// Signature help over unsaved buffer content; same inputs as completion.
struct SignatureHelpParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;

    std::string text;
    uint32_t offset = 0;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;

    Config config;
};

/// Format a document (or a byte range of it) with clang-format.
struct FormatParams {
    std::string file;
    std::string text;
    LocalSourceRange range;  ///< Invalid range = full document.
};

/// Result of an artifact build (PCH or PCM).
struct ArtifactBuildResult {
    bool success = true;
    std::string error;
    /// On failure: whether `error` carries user-code compile errors. A failure
    /// without user errors indicates clice infrastructure breakage (anomaly).
    bool has_user_errors = false;

    /// The tmp path the artifact was written to.
    std::string output_path;

    /// Milliseconds since epoch, sampled before the build started. Files
    /// whose mtime is past this moment may differ from what the build read.
    std::int64_t build_at = 0;
    std::vector<DepFile> deps;
};

/// One clang-tidy finding, located for CLI presentation (1-based line and
/// column; the column counts bytes, like the compiler's).
struct TidyDiagnostic {
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    /// Error (warning otherwise): the check is in WarningsAsErrors.
    bool error = false;

    std::string message;

    /// Check name, e.g. "bugprone-integer-division".
    std::string check;
};

struct TURunResult {
    bool success = true;
    std::string error;
    /// See ArtifactBuildResult::has_user_errors.
    bool has_user_errors = false;

    /// Serialized TUIndex, merged by the master (plan product `index`).
    std::string tu_index_data;

    /// Findings of the tidy pass (plan product `tidy`).
    std::vector<TidyDiagnostic> tidy_diagnostics;
};

/// Request the document links of an open file's AST. Only the main-file
/// region is covered: the preamble is compiled into the PCH, and its links
/// live in the PCH's pch.idx envelope (spliced in by the master).
struct DocumentLinkParams {
    std::string path;
};

struct EvictParams {
    std::string path;
};

struct EvictedParams {
    std::string path;
};

/// Interrupt the in-flight compile of `path`, if any. Sent at the master's
/// supersede point instead of wire-cancelling the compile request: the
/// worker flips the compile's stop flag so clang abandons the stale parse
/// at the next declaration, while the request still runs to a normal
/// (incomplete) reply — the master keeps observing the real outcome, so a
/// worker death during a superseded compile still reaches the document's
/// quarantine accounting.
struct CancelCompileParams {
    std::string path;
};

/// Interrupt a stateless worker's in-flight build. Sent by the pool's
/// cooperative cancel instead of wire-cancelling the build request: the
/// worker flips the build's stop flag so clang abandons the parse at the
/// next declaration, while the request still runs to a normal (cancelled)
/// reply. The sender keeps awaiting that reply, so the slot stays busy —
/// and the cancel-grace deadline stays armed — until the process is
/// actually free; a wire cancel would resume the sender immediately and
/// hand the slot out while the worker is still stuck in the old parse.
/// Carries no build identity: the pool dispatches at most one build per
/// worker at a time, and pipe ordering pins any follow-up build behind
/// the cancel.
struct CancelBuildParams {};

}  // namespace clice::worker

namespace kota::ipc::protocol {

template <>
struct RequestTraits<clice::worker::CompileParams> {
    using Result = clice::worker::CompileResult;
    constexpr inline static std::string_view method = "clice/worker/compile";
};

template <>
struct RequestTraits<clice::worker::QueryParams> {
    using Result = kota::codec::RawValue;
    constexpr inline static std::string_view method = "clice/worker/query";
};

template <>
struct RequestTraits<clice::worker::DocumentLinkParams> {
    using Result = std::vector<clice::feature::DocumentLink>;
    constexpr inline static std::string_view method = "clice/worker/documentLink";
};

template <>
struct RequestTraits<clice::worker::BuildPCHParams> {
    using Result = clice::worker::ArtifactBuildResult;
    constexpr inline static std::string_view method = "clice/worker/buildPch";
};

template <>
struct RequestTraits<clice::worker::BuildPCMParams> {
    using Result = clice::worker::ArtifactBuildResult;
    constexpr inline static std::string_view method = "clice/worker/buildPcm";
};

template <>
struct RequestTraits<clice::worker::TURunParams> {
    using Result = clice::worker::TURunResult;
    constexpr inline static std::string_view method = "clice/worker/tuRun";
};

template <>
struct RequestTraits<clice::worker::CompletionParams> {
    using Result = kota::codec::RawValue;
    constexpr inline static std::string_view method = "clice/worker/completion";
};

template <>
struct RequestTraits<clice::worker::SignatureHelpParams> {
    using Result = kota::codec::RawValue;
    constexpr inline static std::string_view method = "clice/worker/signatureHelp";
};

template <>
struct RequestTraits<clice::worker::FormatParams> {
    using Result = kota::codec::RawValue;
    constexpr inline static std::string_view method = "clice/worker/format";
};

template <>
struct NotificationTraits<clice::worker::EvictParams> {
    constexpr inline static std::string_view method = "clice/worker/evict";
};

template <>
struct NotificationTraits<clice::worker::EvictedParams> {
    constexpr inline static std::string_view method = "clice/worker/evicted";
};

template <>
struct NotificationTraits<clice::worker::CancelCompileParams> {
    constexpr inline static std::string_view method = "clice/worker/cancelCompile";
};

template <>
struct NotificationTraits<clice::worker::CancelBuildParams> {
    constexpr inline static std::string_view method = "clice/worker/cancelBuild";
};

}  // namespace kota::ipc::protocol
