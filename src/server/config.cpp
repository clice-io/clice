#define TOML_EXCEPTIONS 0
#include "server/config.h"

#include <type_traits>

#include "support/filesystem.h"

#include "toml++/toml.hpp"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Path.h"

namespace clice::server {

namespace {

constexpr std::string_view k_server_version = "0.1.0";
constexpr std::string_view k_llvm_version = LLVM_VERSION_STRING;

auto resolve_string(std::string& input, const ServerConfig& config) -> void {
    std::string_view text = input;
    std::string output;
    output.reserve(input.size() + 32);

    std::size_t pos = 0;
    while((pos = text.find("${", pos)) != std::string::npos) {
        output.append(text.substr(0, pos));

        auto end = text.find('}', pos + 2);
        if(end == std::string::npos) {
            output.append(text.substr(pos));
            break;
        }

        auto variable = text.substr(pos + 2, end - (pos + 2));
        if(variable == "workspace") {
            output.append(config.workspace);
        } else if(variable == "version") {
            output.append(k_server_version);
        } else if(variable == "llvm_version") {
            output.append(k_llvm_version);
        } else {
            output.append(text.substr(pos, end - pos + 1));
        }

        text.remove_prefix(end + 1);
        pos = 0;
    }
    output.append(text);

    llvm::SmallString<256> normalized{llvm::StringRef(output)};
    llvm::sys::path::remove_dots(normalized, true);
    input = normalized.str().str();
}

template <typename Object>
auto replace_variables(Object& object, const ServerConfig& config) -> void {
    if constexpr(std::is_same_v<Object, std::string>) {
        resolve_string(object, config);
    } else if constexpr(std::is_same_v<Object, ProjectConfig>) {
        replace_variables(object.cache_dir, config);
        replace_variables(object.index_dir, config);
        replace_variables(object.logging_dir, config);
        replace_variables(object.compile_commands_paths, config);
    } else if constexpr(std::is_same_v<Object, RuleConfig>) {
        replace_variables(object.patterns, config);
        replace_variables(object.remove, config);
        replace_variables(object.append, config);
    } else if constexpr(std::is_same_v<Object, ServerConfig>) {
        replace_variables(object.workspace, config);
        replace_variables(object.project, config);
        replace_variables(object.rules, config);
    } else if constexpr(requires {
                            object.begin();
                            object.end();
                        }) {
        for(auto& item: object) {
            replace_variables(item, config);
        }
    }
}

auto parse_bool(bool& output, const toml::node& node) -> void {
    if(auto value = node.as_boolean()) {
        output = value->get();
    }
}

template <typename Int>
auto parse_integral(Int& output, const toml::node& node) -> void {
    if(auto value = node.as_integer()) {
        output = static_cast<Int>(value->get());
    }
}

auto parse_string(std::string& output, const toml::node& node) -> void {
    if(auto value = node.as_string()) {
        output = value->get();
    }
}

auto parse_string_list(std::vector<std::string>& output, const toml::node& node) -> void {
    auto values = node.as_array();
    if(!values) {
        return;
    }
    for(auto& value: *values) {
        if(auto item = value.as_string()) {
            output.emplace_back(item->get());
        }
    }
}

auto parse_rule(RuleConfig& output, const toml::node& node) -> void {
    auto table = node.as_table();
    if(!table) {
        return;
    }

    if(auto value = (*table)["patterns"]; auto node_ptr = value.node()) {
        parse_string_list(output.patterns, *node_ptr);
    }
    if(auto value = (*table)["remove"]; auto node_ptr = value.node()) {
        parse_string_list(output.remove, *node_ptr);
    }
    if(auto value = (*table)["append"]; auto node_ptr = value.node()) {
        parse_string_list(output.append, *node_ptr);
    }
}

auto parse_project(ProjectConfig& output, const toml::node& node) -> void {
    auto table = node.as_table();
    if(!table) {
        return;
    }

    if(auto value = (*table)["root"]; auto node_ptr = value.node()) {
        parse_bool(output.root, *node_ptr);
    }
    if(auto value = (*table)["clang_tidy"]; auto node_ptr = value.node()) {
        parse_bool(output.clang_tidy, *node_ptr);
    }
    if(auto value = (*table)["max_active_file"]; auto node_ptr = value.node()) {
        parse_integral(output.max_active_file, *node_ptr);
    }
    if(auto value = (*table)["cache_dir"]; auto node_ptr = value.node()) {
        parse_string(output.cache_dir, *node_ptr);
    }
    if(auto value = (*table)["index_dir"]; auto node_ptr = value.node()) {
        parse_string(output.index_dir, *node_ptr);
    }
    if(auto value = (*table)["logging_dir"]; auto node_ptr = value.node()) {
        parse_string(output.logging_dir, *node_ptr);
    }
    if(auto value = (*table)["compile_commands_paths"]; auto node_ptr = value.node()) {
        parse_string_list(output.compile_commands_paths, *node_ptr);
    }
}

auto parse_file(ServerConfig& output, const toml::table& table) -> void {
    if(auto value = table["project"]; auto node_ptr = value.node()) {
        parse_project(output.project, *node_ptr);
    }

    if(auto value = table["rules"]; auto node_ptr = value.node()) {
        if(auto rules = node_ptr->as_array()) {
            for(auto& item: *rules) {
                auto& rule = output.rules.emplace_back();
                parse_rule(rule, item);
            }
        }
    }
}

}  // namespace

auto ServerConfig::parse(std::string_view workspace_path) -> std::expected<void, std::string> {
    workspace = std::string(workspace_path);
    auto file = path::join(workspace, "clice.toml");

    std::string error_message;
    if(fs::exists(file)) {
        auto parsed = toml::parse_file(file);
        if(parsed) {
            parse_file(*this, parsed.table());
        } else {
            error_message = parsed.error().description();
        }
    } else {
        error_message = "Config file doesn't exist!";
    }

    replace_variables(*this, *this);

    if(!error_message.empty()) {
        return std::unexpected(std::move(error_message));
    }
    return {};
}

}  // namespace clice::server
