#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "eventide/ipc/protocol.h"

#include "llvm/ADT/StringMap.h"

namespace clice::worker {

// === StatelessWorker requests (Master → StatelessWorker) ===

struct CompletionParams {
    std::string uri;
    int version = 0;
    std::string text;
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, std::uint32_t> pch;
    std::vector<std::pair<std::string, std::string>> pcms;
    int line = 0;
    int character = 0;
};

struct CompletionResult {
    std::string json_result;
};

struct SignatureHelpParams {
    std::string uri;
    int version = 0;
    std::string text;
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, std::uint32_t> pch;
    std::vector<std::pair<std::string, std::string>> pcms;
    int line = 0;
    int character = 0;
};

struct SignatureHelpResult {
    std::string json_result;
};

struct BuildPCHParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    std::string content;
};

struct BuildPCHResult {
    bool success = false;
    std::string error;
};

struct BuildPCMParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    std::string module_name;
    std::vector<std::pair<std::string, std::string>> pcms;
};

struct BuildPCMResult {
    bool success = false;
    std::string error;
};

struct IndexParams {
    std::string file;
    std::string directory;
    std::vector<std::string> arguments;
    std::vector<std::pair<std::string, std::string>> pcms;
};

struct IndexResult {
    bool success = false;
    std::string error;
    std::string tu_index_data;
};

// === StatefulWorker requests (Master → StatefulWorker) ===

struct DocumentCompileParams {
    std::string uri;
    int version = 0;
    std::string text;
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, std::uint32_t> pch;
    std::vector<std::pair<std::string, std::string>> pcms;
    bool clang_tidy = false;
};

struct DocumentCompileResult {
    std::string diagnostics_json;
};

struct HoverParams {
    std::string uri;
    int line = 0;
    int character = 0;
};

struct HoverResult {
    std::string json_result;
};

struct SemanticTokensParams {
    std::string uri;
};

struct SemanticTokensResult {
    std::string json_result;
};

struct DocumentSymbolsParams {
    std::string uri;
};

struct DocumentSymbolsResult {
    std::string json_result;
};

struct FoldingRangesParams {
    std::string uri;
};

struct FoldingRangesResult {
    std::string json_result;
};

struct DocumentLinksParams {
    std::string uri;
};

struct DocumentLinksResult {
    std::string json_result;
};

struct InlayHintsParams {
    std::string uri;
    int start_line = 0;
    int start_character = 0;
    int end_line = 0;
    int end_character = 0;
};

struct InlayHintsResult {
    std::string json_result;
};

// === Notifications ===

struct EvictParams {
    std::string uri;
};

struct EvictedParams {
    std::string uri;
};

}  // namespace clice::worker

// EventIDE RequestTraits specializations for StatelessWorker requests
namespace eventide::ipc::protocol {

template <>
struct RequestTraits<clice::worker::CompletionParams> {
    using Result = clice::worker::CompletionResult;
    static constexpr std::string_view method = "clice/worker/completion";
};

template <>
struct RequestTraits<clice::worker::SignatureHelpParams> {
    using Result = clice::worker::SignatureHelpResult;
    static constexpr std::string_view method = "clice/worker/signatureHelp";
};

template <>
struct RequestTraits<clice::worker::BuildPCHParams> {
    using Result = clice::worker::BuildPCHResult;
    static constexpr std::string_view method = "clice/worker/buildPCH";
};

template <>
struct RequestTraits<clice::worker::BuildPCMParams> {
    using Result = clice::worker::BuildPCMResult;
    static constexpr std::string_view method = "clice/worker/buildPCM";
};

template <>
struct RequestTraits<clice::worker::IndexParams> {
    using Result = clice::worker::IndexResult;
    static constexpr std::string_view method = "clice/worker/index";
};

// StatefulWorker requests
template <>
struct RequestTraits<clice::worker::DocumentCompileParams> {
    using Result = clice::worker::DocumentCompileResult;
    static constexpr std::string_view method = "clice/worker/compile";
};

template <>
struct RequestTraits<clice::worker::HoverParams> {
    using Result = clice::worker::HoverResult;
    static constexpr std::string_view method = "clice/worker/hover";
};

template <>
struct RequestTraits<clice::worker::SemanticTokensParams> {
    using Result = clice::worker::SemanticTokensResult;
    static constexpr std::string_view method = "clice/worker/semanticTokens";
};

template <>
struct RequestTraits<clice::worker::DocumentSymbolsParams> {
    using Result = clice::worker::DocumentSymbolsResult;
    static constexpr std::string_view method = "clice/worker/documentSymbols";
};

template <>
struct RequestTraits<clice::worker::FoldingRangesParams> {
    using Result = clice::worker::FoldingRangesResult;
    static constexpr std::string_view method = "clice/worker/foldingRanges";
};

template <>
struct RequestTraits<clice::worker::DocumentLinksParams> {
    using Result = clice::worker::DocumentLinksResult;
    static constexpr std::string_view method = "clice/worker/documentLinks";
};

template <>
struct RequestTraits<clice::worker::InlayHintsParams> {
    using Result = clice::worker::InlayHintsResult;
    static constexpr std::string_view method = "clice/worker/inlayHints";
};

// Notification traits
template <>
struct NotificationTraits<clice::worker::EvictParams> {
    static constexpr std::string_view method = "clice/worker/evict";
};

template <>
struct NotificationTraits<clice::worker::EvictedParams> {
    static constexpr std::string_view method = "clice/worker/evicted";
};

}  // namespace eventide::ipc::protocol
