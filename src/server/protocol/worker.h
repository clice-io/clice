#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "feature/document_link.h"
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

}  // namespace dispatch_errc

/// True when a dispatch failure is an expected operational condition rather
/// than clice infrastructure breakage.
inline bool is_operational_error(const protocol::Error& error) {
    return error.code == dispatch_errc::cancelled ||
           error.code == dispatch_errc::worker_unavailable;
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

struct CompileResult {
    int version;
    /// Diagnostics serialized as JSON (RawValue) to avoid bincode/serde annotation conflicts.
    kota::codec::RawValue diagnostics;
    std::size_t memory_usage;
    std::vector<std::string> deps;
    /// Serialized TUIndex for the main file (interested_only=true).
    std::string tu_index_data;

    /// Preprocessor-inactive regions as flat byte-offset pairs
    /// [begin0, end0, begin1, end1, ...] in the main file. Covers only
    /// the content past the PCH bound; the preamble's share lives in
    /// PCHState (analogous to document links).
    std::vector<std::uint32_t> inactive_regions;
};

enum class Priority : uint8_t { High, Low };

/// Kind of build task dispatched to a stateless worker.
enum class BuildKind : uint8_t {
    BuildPCH,
    BuildPCM,
    Index,
    Completion,
    SignatureHelp,
    Format,
};

/// Unified parameters for all stateless build/compilation tasks.
/// Fields are used selectively based on `kind`:
///   - All:           file, directory, arguments
///   - BuildPCH:      + content, preamble_bound, output_path
///   - BuildPCM:      + module_name, pcms, output_path
///   - Index:         + pcms
///   - Completion:    + text, version, offset, pch, pcms
///   - SignatureHelp: + text, version, offset, pch, pcms
///   - Format:        + text, format_range (optional)
struct BuildParams {
    // FIXME: BuildPCM dispatched via compile_graph defaults to Low, which can
    // starve interactive dep-resolution (hover/completion) behind indexing.
    // Consider routing module-dep builds as High when triggered by a user request.
    Priority priority = Priority::Low;
    BuildKind kind;
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;

    /// Source text for Completion/SignatureHelp, preamble content for BuildPCH.
    std::string text;
    int version = 0;
    uint32_t offset = 0;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;

    std::string output_path;               ///< BuildPCH, BuildPCM
    std::string module_name;               ///< BuildPCM
    uint32_t preamble_bound = UINT32_MAX;  ///< BuildPCH
    LocalSourceRange format_range;         ///< Format (default = full document)
};

/// Unified result for stateless build tasks.
/// For Completion/SignatureHelp, the result JSON is in `result_json`.
/// For BuildPCH/BuildPCM/Index, structured fields are used.
struct BuildResult {
    bool success = true;
    std::string error;
    /// On failure: whether `error` carries user-code compile errors. A failure
    /// without user errors indicates clice infrastructure breakage (anomaly).
    bool has_user_errors = false;
    std::string output_path;  ///< PCH or PCM path
    std::vector<std::string> deps;
    std::string tu_index_data;
    /// Include directives of the PCH preamble. Structured so the master can
    /// serve both document links and go-to-definition on preamble lines.
    std::vector<clice::feature::DocumentLink> preamble_links;

    /// Inactive regions within the preamble region (flat offset pairs)
    /// and the conditional stack still open at the bound.
    std::vector<std::uint32_t> inactive_regions;
    std::vector<std::uint8_t> open_conditionals;
    kota::codec::RawValue result_json;  ///< Completion/SignatureHelp result
};

/// Request the document links of an open file's AST. Only the main-file
/// region is covered: the preamble is compiled into the PCH, and its links
/// travel in BuildResult::preamble_links.
struct DocumentLinkParams {
    std::string path;
};

struct EvictParams {
    std::string path;
};

struct EvictedParams {
    std::string path;
};

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
struct RequestTraits<clice::worker::BuildParams> {
    using Result = clice::worker::BuildResult;
    constexpr inline static std::string_view method = "clice/worker/build";
};

template <>
struct NotificationTraits<clice::worker::EvictParams> {
    constexpr inline static std::string_view method = "clice/worker/evict";
};

template <>
struct NotificationTraits<clice::worker::EvictedParams> {
    constexpr inline static std::string_view method = "clice/worker/evicted";
};

}  // namespace kota::ipc::protocol
