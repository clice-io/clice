#include "server/config.h"

#include "support/logging.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "toml++/toml.hpp"

namespace clice {

static std::string expand_variables(std::string_view input, std::string_view workspace) {
    std::string result;
    result.reserve(input.size());

    std::size_t pos = 0;
    while(pos < input.size()) {
        auto dollar = input.find("${", pos);
        if(dollar == std::string_view::npos) {
            result.append(input.substr(pos));
            break;
        }
        result.append(input.substr(pos, dollar - pos));
        auto end = input.find('}', dollar + 2);
        if(end == std::string_view::npos) {
            result.append(input.substr(dollar));
            break;
        }
        auto var_name = input.substr(dollar + 2, end - dollar - 2);
        if(var_name == "workspace") {
            result.append(workspace);
        } else if(var_name == "version") {
            result.append("0.1.0");
        } else if(var_name == "llvm_version") {
            result.append("21");
        } else {
            result.append(input.substr(dollar, end - dollar + 1));
        }
        pos = end + 1;
    }
    return result;
}

Config Config::load(llvm::StringRef workspace_root) {
    Config config;
    std::string ws = workspace_root.str();

    config.project.cache_dir = ws + "/.clice/cache";
    config.project.index_dir = ws + "/.clice/index";
    config.project.logging_dir = ws + "/.clice/logging";
    config.project.compile_commands_paths.push_back(ws + "/build");

    llvm::SmallString<256> toml_path(workspace_root);
    llvm::sys::path::append(toml_path, "clice.toml");

    if(!llvm::sys::fs::exists(toml_path)) {
        LOG_DEBUG("No clice.toml found at {}, using defaults", toml_path.str().str());
        return config;
    }

    auto parse_result = toml::parse_file(std::string_view(toml_path.c_str()));
    if(!parse_result) {
        LOG_WARN("Failed to parse clice.toml: {}", parse_result.error().description());
        return config;
    }

    auto& tbl = parse_result.table();

    if(auto project = tbl["project"].as_table()) {
        if(auto v = (*project)["clang_tidy"].value<bool>()) {
            config.project.clang_tidy = *v;
        }
        if(auto v = (*project)["max_active_file"].value<int64_t>()) {
            config.project.max_active_file = static_cast<std::size_t>(*v);
        }
        if(auto v = (*project)["cache_dir"].value<std::string>()) {
            config.project.cache_dir = expand_variables(*v, ws);
        }
        if(auto v = (*project)["index_dir"].value<std::string>()) {
            config.project.index_dir = expand_variables(*v, ws);
        }
        if(auto v = (*project)["logging_dir"].value<std::string>()) {
            config.project.logging_dir = expand_variables(*v, ws);
        }
        if(auto arr = (*project)["compile_commands_paths"].as_array()) {
            config.project.compile_commands_paths.clear();
            for(auto& elem : *arr) {
                if(auto s = elem.value<std::string>()) {
                    config.project.compile_commands_paths.push_back(expand_variables(*s, ws));
                }
            }
        }
    }

    if(auto rules_arr = tbl["rules"].as_array()) {
        for(auto& rule_node : *rules_arr) {
            if(auto rule_tbl = rule_node.as_table()) {
                Config::Rule rule;
                if(auto patterns = (*rule_tbl)["patterns"].as_array()) {
                    for(auto& p : *patterns) {
                        if(auto s = p.value<std::string>()) {
                            rule.patterns.push_back(*s);
                        }
                    }
                }
                if(auto append = (*rule_tbl)["append"].as_array()) {
                    for(auto& a : *append) {
                        if(auto s = a.value<std::string>()) {
                            rule.append.push_back(*s);
                        }
                    }
                }
                if(auto remove = (*rule_tbl)["remove"].as_array()) {
                    for(auto& r : *remove) {
                        if(auto s = r.value<std::string>()) {
                            rule.remove.push_back(*s);
                        }
                    }
                }
                config.rules.push_back(std::move(rule));
            }
        }
    }

    return config;
}

}  // namespace clice
