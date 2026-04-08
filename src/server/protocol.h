#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "eventide/ipc/lsp/protocol.h"
#include "eventide/ipc/protocol.h"
#include "eventide/serde/serde/raw_value.h"
#include "syntax/token.h"

namespace clice::worker {

namespace protocol = eventide::ipc::protocol;

/// Kind of AST query dispatched to a stateful worker.
enum class QueryKind : uint8_t {
    Hover,
    GoToDefinition,
    SemanticTokens,
    InlayHints,
    FoldingRange,
    DocumentSymbol,
    DocumentLink,
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
};

struct CompileResult {
    int version;
    /// Diagnostics serialized as JSON (RawValue) to avoid bincode/serde annotation conflicts.
    eventide::serde::RawValue diagnostics;
    std::size_t memory_usage;
    std::vector<std::string> deps;
    /// Serialized TUIndex for the main file (interested_only=true).
    std::string tu_index_data;
};

/// Kind of build task dispatched to a stateless worker.
enum class BuildKind : uint8_t {
    BuildPCH,
    BuildPCM,
    Index,
    Completion,
    SignatureHelp,
};

/// Unified parameters for all stateless build/compilation tasks.
/// Fields are used selectively based on `kind`:
///   - All:           file, directory, arguments
///   - BuildPCH:      + content, preamble_bound, output_path
///   - BuildPCM:      + module_name, pcms, output_path
///   - Index:         + pcms
///   - Completion:    + text, version, offset, pch, pcms
///   - SignatureHelp: + text, version, offset, pch, pcms
struct BuildParams {
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
};

/// Unified result for stateless build tasks.
/// For Completion/SignatureHelp, the result JSON is in `result_json`.
/// For BuildPCH/BuildPCM/Index, structured fields are used.
struct BuildResult {
    bool success = true;
    std::string error;
    std::string output_path;  ///< PCH or PCM path
    std::vector<std::string> deps;
    std::string tu_index_data;
    eventide::serde::RawValue result_json;  ///< Completion/SignatureHelp result
};

struct DocumentUpdateParams {
    std::string path;
    int version;
};

struct EvictParams {
    std::string path;
};

struct EvictedParams {
    std::string path;
};

}  // namespace clice::worker

namespace clice::ext {

struct ContextItem {
    std::string label;
    std::string description;
    std::string uri;
};

struct QueryContextParams {
    std::string uri;
    std::optional<int> offset;
};

struct QueryContextResult {
    std::vector<ContextItem> contexts;
    int total;
};

struct CurrentContextParams {
    std::string uri;
};

struct CurrentContextResult {
    std::optional<ContextItem> context;
};

struct SwitchContextParams {
    std::string uri;
    std::string context_uri;
};

struct SwitchContextResult {
    bool success;
};

}  // namespace clice::ext

namespace eventide::ipc::protocol {

template <>
struct RequestTraits<clice::worker::CompileParams> {
    using Result = clice::worker::CompileResult;
    constexpr inline static std::string_view method = "clice/worker/compile";
};

template <>
struct RequestTraits<clice::worker::QueryParams> {
    using Result = eventide::serde::RawValue;
    constexpr inline static std::string_view method = "clice/worker/query";
};

template <>
struct RequestTraits<clice::worker::BuildParams> {
    using Result = clice::worker::BuildResult;
    constexpr inline static std::string_view method = "clice/worker/build";
};

template <>
struct NotificationTraits<clice::worker::DocumentUpdateParams> {
    constexpr inline static std::string_view method = "clice/worker/documentUpdate";
};

template <>
struct NotificationTraits<clice::worker::EvictParams> {
    constexpr inline static std::string_view method = "clice/worker/evict";
};

template <>
struct NotificationTraits<clice::worker::EvictedParams> {
    constexpr inline static std::string_view method = "clice/worker/evicted";
};

}  // namespace eventide::ipc::protocol
