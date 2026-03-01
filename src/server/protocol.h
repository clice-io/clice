#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eventide/jsonrpc/protocol.h"
#include "eventide/language/protocol.h"

namespace clice::server {

namespace et = eventide;
namespace jsonrpc = et::jsonrpc;
namespace rpc = jsonrpc::protocol;

constexpr inline std::string_view k_worker_mode = "--worker";

struct WorkerCompileParams {
    std::string uri;
    int version = 0;
    std::string text;
};

struct WorkerCompileResult {
    std::string uri;
    int version = 0;
    std::vector<rpc::Diagnostic> diagnostics;
};

struct WorkerGetCompileRecipeParams {
    std::string uri;
    std::uint64_t known_revision = 0;
    std::string known_source_path;
};

struct WorkerGetCompileRecipeResult {
    std::string source_path;
    std::uint64_t revision = 0;
    bool unchanged = false;
    bool arguments_from_database = false;
    std::string directory;
    std::vector<std::string> arguments;
};

struct WorkerHoverParams {
    std::string uri;
    int version = 0;
    std::string text;
    int line = 0;
    int character = 0;
};

struct WorkerHoverResult {
    rpc::RequestTraits<rpc::HoverParams>::Result result = std::nullopt;
};

struct WorkerCompletionParams {
    std::string uri;
    int version = 0;
    std::string text;
    int line = 0;
    int character = 0;
    std::optional<std::string> pch_path;
    std::uint32_t pch_preamble_bound = 0;
};

struct WorkerCompletionResult {
    rpc::RequestTraits<rpc::CompletionParams>::Result result = nullptr;
};

struct WorkerSignatureHelpParams {
    std::string uri;
    int version = 0;
    std::string text;
    int line = 0;
    int character = 0;
    std::optional<std::string> pch_path;
    std::uint32_t pch_preamble_bound = 0;
};

struct WorkerSignatureHelpResult {
    rpc::RequestTraits<rpc::SignatureHelpParams>::Result result = std::nullopt;
};

struct WorkerBuildPCHParams {
    std::string uri;
    std::optional<std::string> text;
    std::string output_path;
};

struct WorkerBuildPCHResult {
    bool built = false;
    std::string output_path;
    std::vector<rpc::Diagnostic> diagnostics;
};

struct WorkerBuildPCMParams {
    std::string uri;
    std::optional<std::string> text;
    std::string output_path;
};

struct WorkerBuildPCMResult {
    bool built = false;
    std::string output_path;
    std::string source_path;
    std::vector<std::string> modules;
    std::vector<rpc::Diagnostic> diagnostics;
};

struct WorkerBuildIndexParams {
    std::string uri;
    std::optional<std::string> text;
};

struct WorkerBuildIndexResult {
    bool built = false;
    std::uint64_t symbol_count = 0;
    std::uint64_t main_occurrence_count = 0;
    std::uint64_t main_relation_count = 0;
    std::vector<rpc::Diagnostic> diagnostics;
};

struct WorkerEvictParams {
    std::string uri;
};

}  // namespace clice::server

namespace eventide::jsonrpc::protocol {

template <>
struct RequestTraits<clice::server::WorkerCompileParams> {
    using Result = clice::server::WorkerCompileResult;
    constexpr inline static std::string_view method = "clice/worker/compile";
};

template <>
struct RequestTraits<clice::server::WorkerGetCompileRecipeParams> {
    using Result = clice::server::WorkerGetCompileRecipeResult;
    constexpr inline static std::string_view method = "clice/master/getCompileRecipe";
};

template <>
struct RequestTraits<clice::server::WorkerHoverParams> {
    using Result = clice::server::WorkerHoverResult;
    constexpr inline static std::string_view method = "clice/worker/hover";
};

template <>
struct RequestTraits<clice::server::WorkerCompletionParams> {
    using Result = clice::server::WorkerCompletionResult;
    constexpr inline static std::string_view method = "clice/worker/completion";
};

template <>
struct RequestTraits<clice::server::WorkerSignatureHelpParams> {
    using Result = clice::server::WorkerSignatureHelpResult;
    constexpr inline static std::string_view method = "clice/worker/signatureHelp";
};

template <>
struct RequestTraits<clice::server::WorkerBuildPCHParams> {
    using Result = clice::server::WorkerBuildPCHResult;
    constexpr inline static std::string_view method = "clice/worker/buildPCH";
};

template <>
struct RequestTraits<clice::server::WorkerBuildPCMParams> {
    using Result = clice::server::WorkerBuildPCMResult;
    constexpr inline static std::string_view method = "clice/worker/buildPCM";
};

template <>
struct RequestTraits<clice::server::WorkerBuildIndexParams> {
    using Result = clice::server::WorkerBuildIndexResult;
    constexpr inline static std::string_view method = "clice/worker/buildIndex";
};

template <>
struct NotificationTraits<clice::server::WorkerEvictParams> {
    constexpr inline static std::string_view method = "clice/worker/evict";
};

}  // namespace eventide::jsonrpc::protocol
