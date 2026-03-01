#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace clice::server {

struct ProjectConfig {
    bool root = true;
    bool clang_tidy = false;
    std::size_t max_active_file = 8;

    std::string cache_dir = "${workspace}/.clice/cache";
    std::string index_dir = "${workspace}/.clice/index";
    std::string logging_dir = "${workspace}/.clice/logging";
    std::vector<std::string> compile_commands_paths = {"${workspace}/build"};
};

struct RuleConfig {
    std::vector<std::string> patterns;
    std::vector<std::string> remove;
    std::vector<std::string> append;
};

struct ServerConfig {
    std::string workspace;
    ProjectConfig project;
    std::vector<RuleConfig> rules;

    auto parse(std::string_view workspace_path) -> std::expected<void, std::string>;
};

}  // namespace clice::server
