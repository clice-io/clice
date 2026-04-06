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

namespace clice::worker {

namespace protocol = eventide::ipc::protocol;

// === StatefulWorker Requests ===

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
};

struct HoverParams {
    std::string path;
    uint32_t offset;
};

struct SemanticTokensParams {
    std::string path;
};

struct InlayHintsParams {
    std::string path;
};

struct FoldingRangeParams {
    std::string path;
};

struct DocumentSymbolParams {
    std::string path;
};

struct DocumentLinkParams {
    std::string path;
};

struct CodeActionParams {
    std::string path;
};

struct GoToDefinitionParams {
    std::string path;
    uint32_t offset;
};

// === StatelessWorker Requests ===

struct CompletionParams {
    std::string path;
    int version;
    std::string text;
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;
    uint32_t offset;
};

struct SignatureHelpParams {
    std::string path;
    int version;
    std::string text;
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;
    uint32_t offset;
};

struct BuildPCHParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    std::string content;
    std::uint32_t preamble_bound = UINT32_MAX;
    std::string output_path;
};

struct BuildPCHResult {
    bool success;
    std::string error;
    std::string pch_path;
    std::vector<std::string> deps;
};

struct BuildPCMParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    std::string module_name;
    std::unordered_map<std::string, std::string> pcms;
    std::string output_path;
};

struct BuildPCMResult {
    bool success;
    std::string error;
    std::string pcm_path;
    std::vector<std::string> deps;
};

struct IndexParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    std::unordered_map<std::string, std::string> pcms;
};

struct IndexResult {
    bool success;
    std::string error;
    std::string tu_index_data;
};

// === Notifications ===

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

// === clice/ LSP Extension Types ===

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

// === Stateful Requests ===

template <>
struct RequestTraits<clice::worker::CompileParams> {
    using Result = clice::worker::CompileResult;
    constexpr inline static std::string_view method = "clice/worker/compile";
};

template <>
struct RequestTraits<clice::worker::HoverParams> {
    using Result = eventide::serde::RawValue;
    constexpr inline static std::string_view method = "clice/worker/hover";
};

template <>
struct RequestTraits<clice::worker::SemanticTokensParams> {
    using Result = eventide::serde::RawValue;
    constexpr inline static std::string_view method = "clice/worker/semanticTokens";
};

template <>
struct RequestTraits<clice::worker::InlayHintsParams> {
    using Result = eventide::serde::RawValue;
    constexpr inline static std::string_view method = "clice/worker/inlayHints";
};

template <>
struct RequestTraits<clice::worker::FoldingRangeParams> {
    using Result = eventide::serde::RawValue;
    constexpr inline static std::string_view method = "clice/worker/foldingRange";
};

template <>
struct RequestTraits<clice::worker::DocumentSymbolParams> {
    using Result = eventide::serde::RawValue;
    constexpr inline static std::string_view method = "clice/worker/documentSymbol";
};

template <>
struct RequestTraits<clice::worker::DocumentLinkParams> {
    using Result = eventide::serde::RawValue;
    constexpr inline static std::string_view method = "clice/worker/documentLink";
};

template <>
struct RequestTraits<clice::worker::CodeActionParams> {
    using Result = eventide::serde::RawValue;
    constexpr inline static std::string_view method = "clice/worker/codeAction";
};

template <>
struct RequestTraits<clice::worker::GoToDefinitionParams> {
    using Result = eventide::serde::RawValue;
    constexpr inline static std::string_view method = "clice/worker/goToDefinition";
};

// === Stateless Requests ===

template <>
struct RequestTraits<clice::worker::CompletionParams> {
    using Result = eventide::serde::RawValue;
    constexpr inline static std::string_view method = "clice/worker/completion";
};

template <>
struct RequestTraits<clice::worker::SignatureHelpParams> {
    using Result = eventide::serde::RawValue;
    constexpr inline static std::string_view method = "clice/worker/signatureHelp";
};

template <>
struct RequestTraits<clice::worker::BuildPCHParams> {
    using Result = clice::worker::BuildPCHResult;
    constexpr inline static std::string_view method = "clice/worker/buildPCH";
};

template <>
struct RequestTraits<clice::worker::BuildPCMParams> {
    using Result = clice::worker::BuildPCMResult;
    constexpr inline static std::string_view method = "clice/worker/buildPCM";
};

template <>
struct RequestTraits<clice::worker::IndexParams> {
    using Result = clice::worker::IndexResult;
    constexpr inline static std::string_view method = "clice/worker/index";
};

// === Notifications ===

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
