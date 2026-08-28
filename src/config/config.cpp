#include "config/config.h"

#include <algorithm>
#include <array>

#include "feature/feature.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/async/io/system.h"
#include "kota/codec/json/json.h"
#include "kota/codec/json/schema.h"
#include "kota/codec/toml/toml.h"
#include "kota/support/glob_pattern.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

/// Replace all occurrences of ${workspace} with the workspace root.
/// No-op when workspace_root is empty, to avoid producing paths like "/cache"
/// from "${workspace}/cache".
static void substitute_workspace(std::string& value, llvm::StringRef workspace_root) {
    if(workspace_root.empty())
        return;
    constexpr std::string_view placeholder = "${workspace}";
    std::size_t pos = 0;
    while((pos = value.find(placeholder, pos)) != std::string::npos) {
        value.replace(pos, placeholder.size(), workspace_root);
        pos += workspace_root.size();
    }
}

std::uint32_t default_stateless_worker_count() {
    // Config is constructed on every worker request (QueryParams /
    // build params carry one); query the core count once, not per request.
    const static std::uint32_t count = std::max(kota::sys::parallelism() / 2, 2u);
    return count;
}

std::uint32_t default_max_stateless_worker_count() {
    const static std::uint32_t count = kota::sys::parallelism();
    return count;
}

void Config::finalize(llvm::StringRef workspace_root) {
    auto& p = project;

    // Validation, not default-filling: zero workers or a zero memory
    // budget is never a runnable configuration, so an explicit 0 falls
    // back to the field's born-valid default with a warning.
    ProjectConfig defaults;
    auto reject_zero = [](auto& field, const auto& fallback, llvm::StringRef name) {
        if(field.value == 0) {
            LOG_WARN("{} = 0 is invalid; using {}", name, fallback.value);
            field = fallback.value;
        }
    };
    reject_zero(p.stateful_worker_count, defaults.stateful_worker_count, "stateful_worker_count");
    reject_zero(p.stateless_worker_count,
                defaults.stateless_worker_count,
                "stateless_worker_count");
    reject_zero(p.min_stateless_worker_count,
                defaults.min_stateless_worker_count,
                "min_stateless_worker_count");
    if(p.cache_dir.empty() && !workspace_root.empty()) {
        p.cache_dir = path::join(workspace_root, ".clice");
        p.cache_dir_defaulted = true;
    }
    if(p.logging_dir.empty() && !p.cache_dir.empty())
        p.logging_dir = path::join(p.cache_dir, "logs");

    // Variable substitution on string fields.
    substitute_workspace(p.cache_dir, workspace_root);
    substitute_workspace(p.logging_dir, workspace_root);
    // Client-supplied dirs arrive in native spelling (backslashes, any
    // drive case); canonicalize so artifact-prefix checks against
    // pool-resolved paths hold.
    path::canonicalize(p.cache_dir);
    path::canonicalize(p.logging_dir);
    for(auto& entry: p.compile_commands_paths)
        substitute_workspace(entry, workspace_root);

    // Pre-compile glob patterns from rules.
    compiled_rules.clear();
    for(auto& rule: rules) {
        CompiledRule compiled;
        for(auto& pattern_str: rule.patterns) {
            auto pat = kota::GlobPattern::create(pattern_str);
            if(!pat) {
                LOG_WARN("Invalid glob pattern in rule: {}: {}", pattern_str, pat.error().message);
                continue;
            }
            compiled.patterns.push_back(std::move(*pat));
        }
        // Drop the whole rule if no pattern compiled successfully — otherwise the
        // append/remove flags would be silently attached to a rule that can never match.
        if(compiled.patterns.empty()) {
            if(!rule.patterns.empty())
                LOG_WARN("Rule dropped: all glob patterns failed to compile");
            continue;
        }
        compiled.append.assign(rule.append.begin(), rule.append.end());
        compiled.remove.assign(rule.remove.begin(), rule.remove.end());
        compiled_rules.push_back(std::move(compiled));
    }
}

void Config::match_rules(llvm::StringRef file_path,
                         std::vector<std::string>& append,
                         std::vector<std::string>& remove) const {
    // Rules are processed in declaration order so that a later rule can
    // override an earlier one. Specifically, when a later rule removes
    // an argument, we also strip any string-equal entry already added
    // to `append` by an earlier matching rule — otherwise the append
    // would silently survive (lookup applies removes to the base flags
    // only, not to entries contributed via `append`).
    for(auto& rule: compiled_rules) {
        bool matched =
            std::ranges::any_of(rule.patterns, [&](auto& pat) { return pat.match(file_path); });
        if(!matched)
            continue;

        for(auto& r: rule.remove) {
            std::erase(append, r);
            remove.push_back(r);
        }
        append.insert(append.end(), rule.append.begin(), rule.append.end());
    }
}

/// Codec config that rejects unknown keys: the strict validation pass
/// decodes under it, and the published schema derives its
/// `additionalProperties: false` from it — the same typo surfaces both
/// ways.
struct DenyUnknownKeys {
    constexpr static bool deny_unknown_fields = true;
};

static ConfigIssue make_issue(ConfigIssue::Severity severity,
                              llvm::StringRef path,
                              const kota::codec::rich_error& error) {
    ConfigIssue issue;
    issue.severity = severity;
    issue.file = path.str();
    issue.message = error.to_string();
    if(error.location) {
        issue.line = static_cast<std::uint32_t>(error.location->line);
        issue.column = static_cast<std::uint32_t>(error.location->column);
    }
    return issue;
}

std::optional<Config> Config::load(llvm::StringRef path,
                                   llvm::StringRef workspace_root,
                                   std::vector<ConfigIssue>* issues,
                                   bool finalized) {
    auto content = fs::read(path);
    if(!content)
        return std::nullopt;

    auto result = kota::codec::toml::from_string<Config>(*content);
    if(!result) {
        LOG_ERROR("Invalid clice.toml {}: {}", path, result.error().to_string());
        if(issues)
            issues->push_back(make_issue(ConfigIssue::Severity::Error, path, result.error()));
        return std::nullopt;
    }

    // Second, strict decode pass that rejects unknown keys. The lenient
    // result above still applies — this only surfaces typos (e.g. a
    // misspelled option silently doing nothing) as Warning issues.
    if(issues) {
        Config probe{};
        if(auto strict = kota::codec::toml::from_string<DenyUnknownKeys>(*content, probe);
           !strict) {
            LOG_WARN("clice.toml {}: {}", path, strict.error().to_string());
            issues->push_back(make_issue(ConfigIssue::Severity::Warning, path, strict.error()));
        }
    }

    auto config = std::move(*result);
    if(finalized)
        config.finalize(workspace_root);
    LOG_INFO("Loaded config from {}", path);
    return config;
}

std::optional<Config> Config::load_from_json(llvm::StringRef json, llvm::StringRef workspace_root) {
    Config config{};
    auto result = kota::codec::json::from_string(json, config);
    if(!result) {
        LOG_WARN("Failed to parse initializationOptions JSON: {}", result.error().message);
        return std::nullopt;
    }

    config.finalize(workspace_root);
    LOG_INFO("Loaded config from initializationOptions");
    return config;
}

Config Config::load_from_workspace(llvm::StringRef workspace_root,
                                   std::vector<ConfigIssue>* issues,
                                   std::string* loaded_path,
                                   bool finalized) {
    if(loaded_path)
        loaded_path->clear();

    bool found = false;
    if(!workspace_root.empty()) {
        for(auto* name: {"clice.toml", ".clice/config.toml"}) {
            auto config_path = path::join(workspace_root, name);
            if(!llvm::sys::fs::exists(config_path))
                continue;
            found = true;
            if(loaded_path)
                *loaded_path = config_path;
            if(auto config = load(config_path, workspace_root, issues, finalized))
                return std::move(*config);
            // Present but malformed: fall through to defaults, but surface
            // the situation clearly so users know their config wasn't applied.
            LOG_WARN("Falling back to default configuration because {} is invalid", config_path);
        }
    }

    if(!found) {
        LOG_INFO("No clice.toml found in {}, using default configuration", workspace_root);
    }

    Config config;
    if(finalized) {
        config.finalize(workspace_root);
    }
    return config;
}

constexpr std::array MACHINE_DERIVED_FIELDS = {"stateless_worker_count",
                                               "max_stateless_worker_count"};

/// The fields finalize() rejects `0` for.
constexpr std::array ZERO_INVALID_FIELDS = {"stateful_worker_count",
                                            "stateless_worker_count",
                                            "min_stateless_worker_count"};

/// Scrub the machine-derived fields out of a `default` object: sections
/// carry whole-object defaults, so the values appear below `default`
/// keys too, not only in the fields' own schemas.
static void remove_machine_fields(kota::codec::dyn::Value& value) {
    if(auto* object = value.get_object()) {
        for(auto field: MACHINE_DERIVED_FIELDS) {
            object->remove(field);
        }
        for(auto& [key, child]: *object) {
            remove_machine_fields(child);
        }
    } else if(auto* array = value.get_array()) {
        for(auto& child: *array) {
            remove_machine_fields(child);
        }
    }
}

/// The schema object of `field` inside a `properties` map, if present.
static kota::codec::dyn::Object* field_schema(kota::codec::dyn::Object& properties,
                                              std::string_view field) {
    if(auto* schema = properties.find(field)) {
        return schema->get_object();
    }
    return nullptr;
}

/// Patch the field schemas with what the annotations cannot express:
/// `default`s whose fresh value depends on the running machine are
/// dropped — a committed schema must be byte-identical on every host, so
/// the affected fields' descriptions state the derivation instead — the
/// zero-invalid fields carry the lower bound finalize() enforces, and
/// `index_db` names the backends open_database() accepts, so editors
/// flag a typo that would silently fall back to LMDB.
static void patch_field_schemas(kota::codec::dyn::Value& value) {
    if(auto* object = value.get_object()) {
        for(auto& [key, child]: *object) {
            if(key == "properties") {
                if(auto* properties = child.get_object()) {
                    for(auto field: MACHINE_DERIVED_FIELDS) {
                        if(auto* schema = field_schema(*properties, field)) {
                            schema->remove("default");
                        }
                    }
                    for(auto field: ZERO_INVALID_FIELDS) {
                        if(auto* schema = field_schema(*properties, field)) {
                            schema->assign("minimum", std::uint64_t{1});
                        }
                    }
                    if(auto* schema = field_schema(*properties, "index_db")) {
                        schema->assign("enum", kota::codec::dyn::Array{"lmdb", "files"});
                    }
                    if(auto* schema = field_schema(*properties, "readonly")) {
                        schema->assign("enum", kota::codec::dyn::Array{"off", "on", "auto"});
                    }
                }
            } else if(key == "default") {
                remove_machine_fields(child);
            }
            patch_field_schemas(child);
        }
    } else if(auto* array = value.get_array()) {
        for(auto& child: *array) {
            patch_field_schemas(child);
        }
    }
}

std::expected<std::string, std::string> Config::json_schema() {
    auto schema = kota::codec::json::schema<Config, DenyUnknownKeys>();
    if(!schema) {
        return std::unexpected(schema.error().message);
    }
    patch_field_schemas(*schema);

    auto compact = kota::codec::json::to_string(std::move(*schema));
    if(!compact) {
        return std::unexpected(compact.error().message);
    }
    auto pretty = kota::codec::json::prettify(*compact);
    if(!pretty) {
        return std::unexpected(pretty.error().message);
    }
    return std::move(*pretty);
}

}  // namespace clice
