#pragma once

#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "server/protocol.h"

namespace clice::server {

class CompilationService {
public:
    CompilationService();
    ~CompilationService();

    CompilationService(CompilationService&&) noexcept;
    CompilationService& operator=(CompilationService&&) noexcept;

    CompilationService(const CompilationService&) = delete;
    CompilationService& operator=(const CompilationService&) = delete;

    void set_compile_commands_paths(const std::vector<std::string>& paths);

    auto resolve_recipe(const WorkerGetCompileRecipeParams& params)
        -> std::expected<WorkerGetCompileRecipeResult, std::string>;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace clice::server
