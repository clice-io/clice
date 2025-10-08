#pragma once

#include <vector>
#include <expected>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice::config {

struct ProjectOptions {
    bool root = true;

    bool clang_tidy = false;

    std::size_t max_active_file = 8;

    std::string cache_dir = "${workspace}/.clice/cache";

    std::string index_dir = "${workspace}/.clice/index";

    std::string logging_dir = "${workspace}/.clice/logging";

    std::vector<std::string> compile_commands_dirs = {"${workspace}/build"};
};

struct Rule {
    /// All patterns of the rule.
    llvm::SmallVector<std::string> patterns;

    /// The commands that you want to remove from original command.
    llvm::SmallVector<std::string> remove;

    /// The commands that you want to append from original command.
    llvm::SmallVector<std::string> append;
};

struct Config {
    /// The workspace of this config file.
    std::string workspace;

    /// Project level configs.
    ProjectOptions project;

    /// All rules used for specific files.
    llvm::SmallVector<Rule> rules;

    auto parse(llvm::StringRef workspace) -> std::expected<void, std::string>;
};

};  // namespace clice::config
