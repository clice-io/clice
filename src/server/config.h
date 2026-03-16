#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"

namespace clice {

struct Config {
    struct Project {
        bool clang_tidy = false;
        std::size_t max_active_file = 8;
        std::string cache_dir;
        std::string index_dir;
        std::string logging_dir;
        std::vector<std::string> compile_commands_paths;
    } project;

    struct Rule {
        std::vector<std::string> patterns;
        std::vector<std::string> append;
        std::vector<std::string> remove;
    };
    std::vector<Rule> rules;

    static Config load(llvm::StringRef workspace_root);
};

}  // namespace clice
