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
    std::string uri;
    int version;
    std::string text;
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;
};

struct CompileResult {
    std::string uri;
    int version;
    /// Diagnostics serialized as JSON (RawValue) to avoid bincode/serde annotation conflicts.
    eventide::serde::RawValue diagnostics;
    std::size_t memory_usage;
};

struct HoverParams {
    std::string uri;
    int line;
    int character;
};

struct SemanticTokensParams {
    std::string uri;
};

struct InlayHintsParams {
    std::string uri;
    protocol::Range range;
};

struct FoldingRangeParams {
    std::string uri;
};

struct DocumentSymbolParams {
    std::string uri;
};

struct DocumentLinkParams {
    std::string uri;
};

struct CodeActionParams {
    std::string uri;
    protocol::Range range;
};

struct GoToDefinitionParams {
    std::string uri;
    int line;
    int character;
};

// === StatelessWorker Requests ===

struct CompletionParams {
    std::string uri;
    int version;
    std::string text;
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;
    int line;
    int character;
};

struct SignatureHelpParams {
    std::string uri;
    int version;
    std::string text;
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;
    int line;
    int character;
};

struct BuildPCHParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    std::string content;
};

struct BuildPCHResult {
    bool success;
    std::string error;
};

struct BuildPCMParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    std::string module_name;
    std::unordered_map<std::string, std::string> pcms;
};

struct BuildPCMResult {
    bool success;
    std::string error;
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
    std::string uri;
    int version;
    std::string text;
};

struct EvictParams {
    std::string uri;
};

struct EvictedParams {
    std::string uri;
};

}  // namespace clice::worker

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
