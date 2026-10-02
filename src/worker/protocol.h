#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compile/dep_file.h"
#include "config/config.h"
#include "feature/feature.h"
#include "syntax/preamble_synthesis.h"
#include "syntax/token.h"

#include "kota/codec/json/json.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/protocol.h"
#include "kota/meta/enum.h"

namespace clice::worker {

namespace protocol = kota::ipc::protocol;

/// Error codes attached to master-side dispatch failures. They mark expected
/// operational conditions — memory-pressure preemption, worker deaths and
/// restart windows — as opposed to real IPC breakage: callers must not
/// classify them as anomalies (see support/anomaly.h). The death itself is
/// already reported as a WorkerCrash anomaly by the pool.
///
/// A death fails every request in flight on the worker; the pool tells them
/// apart by the request the dying worker named (see crash_tag), so only the
/// request that crashed it is blamed.
namespace dispatch_errc {

/// The request was deliberately cancelled (memory-pressure preemption).
constexpr inline protocol::integer cancelled =
    static_cast<protocol::integer>(protocol::ErrorCode::RequestCancelled);

/// No live worker could take the request; it was never dispatched.
constexpr inline protocol::integer worker_unavailable = -33000;

/// This request killed its worker: the dying worker named it, or it ran
/// past the pool's deadline. The message says how the worker died.
constexpr inline protocol::integer worker_crashed = -33001;

/// The worker died of another request's crash: this one is blameless and
/// safe to resend.
constexpr inline protocol::integer worker_lost = -33003;

/// The worker died naming no request — killed from outside (the OOM killer,
/// a signal), or crashed where no request was running. Resend once; a
/// request whose resend dies the same way is blamed.
constexpr inline protocol::integer worker_died = -33004;

/// A stateful worker no longer holds the document the query is about (its
/// LRU evicted it): recompile and ask again.
constexpr inline protocol::integer document_unloaded = -33005;

}  // namespace dispatch_errc

/// True when a dispatch failure is an expected operational condition rather
/// than clice infrastructure breakage.
inline bool is_operational_error(const protocol::Error& error) {
    return error.code == dispatch_errc::cancelled ||
           error.code == dispatch_errc::worker_unavailable ||
           error.code == dispatch_errc::worker_crashed ||
           error.code == dispatch_errc::worker_lost || error.code == dispatch_errc::worker_died ||
           error.code == dispatch_errc::document_unloaded;
}

/// The stderr line a dying worker writes to name the request it was
/// running: this prefix, then the request's crash_tag.
constexpr inline std::string_view crashed_in_marker = "clice worker crashed in: ";

/// How a dying worker names a request, and how the master recognizes its
/// own request in that line: the method, a query's kind, and the file.
/// Requests a stateful worker runs side by side differ in one of them
/// unless they are the same work on the same document.
template <typename Params>
std::string crash_tag(const Params& params) {
    std::string tag(protocol::RequestTraits<Params>::method);
    if constexpr(requires { params.kind; }) {
        tag += ':';
        tag += kota::meta::enum_name(params.kind, "Unknown");
    }
    tag += ' ';
    if constexpr(requires { params.path; }) {
        tag += params.path;
    } else {
        tag += params.file;
    }
    return tag;
}

/// Identity of the worker incarnation a crashed request died with, carried
/// in Error::data. One process death fails every request in flight on it;
/// per-content blame (Quarantine) dedups by this identity so a single death
/// is counted at most once per document.
inline kota::codec::dyn::Value death_identity(std::size_t index, unsigned generation, bool stateful) {
    return std::format("{}:{}:{}", stateful ? "sf" : "sl", index, generation);
}

/// The death identity attached to a worker death's error; empty when the
/// error carries none (locally synthesized failures).
inline std::string_view death_of(const protocol::Error& error) {
    if(error.data.has_value()) {
        return error.data->get_string().value_or(std::string_view{});
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
    SemanticTokens,
    InlayHints,
    DocumentSymbol,
};

/// Unified parameters for all stateful AST queries.
/// The worker dispatches to the appropriate feature handler based on `kind`.
struct QueryParams {
    QueryKind kind;
    std::string path;
    uint32_t offset = 0;     ///< Byte offset for position-sensitive queries (Hover).
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
    /// See CompilationParams::workspace.
    std::string workspace;
    std::vector<std::string> arguments;
    /// Files the command names that exist only in memory (path, content):
    /// a header context's synthesized fragments.
    SynthesizedFiles synthesized;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;

    /// Conditional levels left open by the PCH's preamble (1 = branch
    /// inactive), seeding the main compile's inactive-region scan.
    std::vector<std::uint8_t> open_conditionals;

    /// Inactive regions of the preamble share (byte-offset pairs up to the
    /// PCH bound), read from the PCH's pch.idx envelope. The worker only
    /// scans past the bound, yet serves semantic tokens for the whole file.
    std::vector<std::uint32_t> preamble_inactive_regions;
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
    /// Milliseconds since epoch, sampled before the compile started. Files
    /// whose mtime is past this moment may differ from what the build read.
    std::int64_t build_at = 0;
    std::vector<DepFile> deps;
    /// Serialized TUIndex for the main file (main_file_only=true).
    std::string tu_index_data;
};

/// Build a PCH (and its paired pch.idx envelope) from preamble content.
struct BuildPCHParams {
    std::string file;
    std::string directory;
    /// See CompilationParams::workspace.
    std::string workspace;
    std::vector<std::string> arguments;
    /// Files the command names that exist only in memory (path, content):
    /// a header context's synthesized fragments.
    SynthesizedFiles synthesized;

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
    /// See CompilationParams::workspace.
    std::string workspace;
    std::vector<std::string> arguments;
    /// Files the command names that exist only in memory (path, content):
    /// a header context's synthesized fragments.
    SynthesizedFiles synthesized;

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
    /// Files the command names that exist only in memory (path, content):
    /// a header context's synthesized fragments.
    SynthesizedFiles synthesized;

    std::string text;
    uint32_t offset = 0;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;

    /// The workspace config, carried whole — the worker holds no config
    /// state and a config change simply shows up on the next request.
    Config config;

    feature::CompletionClient client;
};

/// Signature help over unsaved buffer content; same inputs as completion.
struct SignatureHelpParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    /// Files the command names that exist only in memory (path, content):
    /// a header context's synthesized fragments.
    SynthesizedFiles synthesized;

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
struct TidyNote {
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string message;

    auto operator<=>(const TidyNote&) const = default;
};

struct TidyDiagnostic {
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    /// Error (warning otherwise): the check is in WarningsAsErrors.
    bool error = false;

    std::string message;

    /// Check name, e.g. "bugprone-integer-division".
    std::string check;

    /// The notes clang-tidy attached ("previous definition is here").
    std::vector<TidyNote> notes;
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

/// Request the folding ranges of an open file's AST. Unlike the links they
/// cover the preamble too: a preamble holds only directives, whose folds
/// come from a lexical scan of the whole file.
struct FoldingRangeParams {
    std::string path;
};

/// Request the code actions of an open file's AST on a byte range of its
/// text: fully computed against the worker's AST, index requests included
/// (see feature::CodeAction).
struct CodeActionParams {
    std::string path;
    LocalSourceRange range;
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

/// Whether a request builds — a compile, a PCH or PCM, an indexing run:
/// work whose time grows with the translation unit, where a query's never
/// should.
template <typename Params>
constexpr inline bool is_build =
    std::same_as<Params, CompileParams> || std::same_as<Params, BuildPCHParams> ||
    std::same_as<Params, BuildPCMParams> || std::same_as<Params, TURunParams>;

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
struct RequestTraits<clice::worker::FoldingRangeParams> {
    /// Empty without an AST: the client then folds by its own means.
    using Result = std::optional<std::vector<clice::feature::FoldingRange>>;
    constexpr inline static std::string_view method = "clice/worker/foldingRange";
};

template <>
struct RequestTraits<clice::worker::CodeActionParams> {
    using Result = std::vector<clice::feature::CodeAction>;
    constexpr inline static std::string_view method = "clice/worker/codeAction";
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
