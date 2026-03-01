#pragma once

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
};

struct WorkerSignatureHelpResult {
    rpc::RequestTraits<rpc::SignatureHelpParams>::Result result = std::nullopt;
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
struct NotificationTraits<clice::server::WorkerEvictParams> {
    constexpr inline static std::string_view method = "clice/worker/evict";
};

}  // namespace eventide::jsonrpc::protocol
