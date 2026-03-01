#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eventide/async/task.h"
#include "eventide/jsonrpc/peer.h"
#include "server/protocol.h"

namespace clice::server {

class WorkerRuntime {
public:
    virtual ~WorkerRuntime() = default;
};

auto make_stateful_worker_runtime(eventide::jsonrpc::Peer& peer, std::size_t document_capacity)
    -> std::unique_ptr<WorkerRuntime>;

auto make_stateless_worker_runtime(eventide::jsonrpc::Peer& peer) -> std::unique_ptr<WorkerRuntime>;

struct CompileCommand {
    bool arguments_from_database = false;
    std::string directory;
    std::vector<std::string> owned_arguments;
    std::vector<const char*> arguments;

    void finalize();
};

struct RecipeState {
    std::uint64_t recipe_revision = 0;
    std::string source_path;
    std::optional<WorkerGetCompileRecipeResult> recipe;
};

auto make_compile_command(const WorkerGetCompileRecipeResult& recipe)
    -> std::expected<CompileCommand, std::string>;

auto resolve_compile_command(eventide::jsonrpc::Peer& peer,
                             std::string_view uri,
                             RecipeState& recipe_state)
    -> eventide::task<std::expected<CompileCommand, std::string>>;

auto to_offset_utf16(std::string_view text, int line, int character) -> std::uint32_t;

}  // namespace clice::server
