#define TOML_EXCEPTIONS 0
#include "toml++/toml.hpp"

#include "Server/Config.h"
#include "Server/Version.h"
#include "Support/Logging.h"
#include "Support/Ranges.h"
#include "Support/FileSystem.h"
#include "llvm/ADT/StringMap.h"

namespace clice::config {

/// replace all predefined variables in the text.
static void resolve(std::string& input, Config& config) {
    std::string_view text = input;
    llvm::SmallString<128> path;
    std::size_t pos = 0;
    while((pos = text.find("${", pos)) != std::string::npos) {
        path.append(text.substr(0, pos));

        auto end = text.find('}', pos + 2);
        if(end == std::string::npos) {
            path.append(text.substr(pos));
            break;
        }

        auto variable = text.substr(pos + 2, end - (pos + 2));

        if(variable == "workspace") {
            path.append(config.workspace);
        } else if(variable == "version") {
            path.append(config::version);
        } else if(variable == "llvm_version") {
            path.append(config::llvm_version);
        } else {
            path.append(text.substr(pos, end - pos + 1));
        }

        text.remove_prefix(end + 1);
        pos = 0;
    }

    path.append(text);
    path::remove_dots(path, true);
    input = path.str();
}

template <typename Object>
static void replace(Object& object, Config& config) {
    if constexpr(std::is_same_v<Object, std::string>) {
        resolve(object, config);
    } else if constexpr(sequence_range<Object>) {
        for(auto& item: object) {
            replace(item, config);
        }
    } else if constexpr(refl::reflectable_struct<Object>) {
        refl::foreach(object, [&](auto, auto& member) { replace(member, config); });
    }
}

template <typename Object>
static void parse_toml(Object& object, auto&& value, Config& config) {
    if constexpr(std::is_same_v<Object, bool>) {
        if(auto v = value.as_boolean()) {
            object = v->get();
        }
    } else if constexpr(clice::integral<Object>) {
        if(auto v = value.as_integer()) {
            object = v->get();
        }
    } else if constexpr(std::is_same_v<Object, std::string>) {
        if(auto v = value.as_string()) {
            object = v->get();
        }
    } else if constexpr(sequence_range<Object>) {
        if(auto v = value.as_array()) {
            for(auto& item: *v) {
                object.emplace_back();
                parse_toml(object.back(), item, config);
            }
        }
    } else if constexpr(refl::reflectable_struct<Object>) {
        if(auto table = value.as_table()) {
            refl::foreach(object, [&](std::string_view key, auto& member) {
                if(auto v = (*table)[key]) {
                    parse_toml(member, v, config);
                }
            });
        }
    } else {
        static_assert(dependent_false<Object>, "Unsupported type");
    }
}

auto Config::parse(llvm::StringRef workspace) -> std::expected<void, std::string> {
    this->workspace = workspace;

    auto path = path::join(workspace, "clice.toml");

    std::string error_message;
    if(fs::exists(path)) {
        if(auto toml = toml::parse_file(path)) {
            parse_toml(*this, toml.table(), *this);
        } else {
            error_message = toml.error().description();
        }
    } else {
        error_message = "Config file doesn't exist!";
    }

    replace(*this, *this);

    if(!error_message.empty()) {
        return std::unexpected(std::move(error_message));
    }

    return {};
}

}  // namespace clice::config
