#pragma once

#include <expected>
#include <memory>
#include <string_view>
#include <vector>

#include "eventide/async/loop.h"
#include "eventide/async/task.h"
#include "eventide/jsonrpc/peer.h"
#include "server/protocol.h"
#include "server/runtime.h"

namespace clice::server {

namespace et = eventide;
namespace jsonrpc = et::jsonrpc;

class WorkerPool {
public:
    WorkerPool(et::event_loop& loop, const Options& options);
    ~WorkerPool();

    WorkerPool(WorkerPool&&) noexcept;
    WorkerPool& operator=(WorkerPool&&) noexcept;

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void set_compile_commands_paths(const std::vector<std::string>& paths);

    auto start() -> std::expected<void, std::string>;

    auto compile(WorkerCompileParams params) -> et::task<jsonrpc::Result<WorkerCompileResult>>;
    auto hover(WorkerHoverParams params) -> et::task<jsonrpc::Result<WorkerHoverResult>>;
    auto completion(WorkerCompletionParams params)
        -> et::task<jsonrpc::Result<WorkerCompletionResult>>;
    auto signature_help(WorkerSignatureHelpParams params)
        -> et::task<jsonrpc::Result<WorkerSignatureHelpResult>>;
    auto build_pch(WorkerBuildPCHParams params) -> et::task<jsonrpc::Result<WorkerBuildPCHResult>>;
    auto build_pcm(WorkerBuildPCMParams params) -> et::task<jsonrpc::Result<WorkerBuildPCMResult>>;
    auto build_index(WorkerBuildIndexParams params)
        -> et::task<jsonrpc::Result<WorkerBuildIndexResult>>;
    auto shutdown() -> et::task<>;

    void release_document(std::string_view uri);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace clice::server
