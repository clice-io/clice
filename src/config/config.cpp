#include "config/config.h"

#include <algorithm>
#include <array>
#include <initializer_list>

#include "feature/feature.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/shell.h"

#include "kota/async/io/system.h"
#include "kota/codec/json/json.h"
#include "kota/codec/json/schema.h"
#include "kota/codec/toml/toml.h"
#include "kota/support/glob_pattern.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
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

/// A literal path as the glob that matches only itself.
static std::string glob_escape(llvm::StringRef literal) {
    std::string escaped;
    for(char c: literal) {
        if(llvm::StringRef(R"(*?[]{}\)").contains(c)) {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

/// Compile one pattern against absolute paths: the literal directory before
/// the first wildcard segment is anchored (a relative one at `anchor`),
/// dot-normalized and canonicalized, and the wildcard tail follows verbatim.
/// A `**`-led pattern matches anywhere and enumerates from the workspace.
/// `workspace_root` must be canonical: substituted into glob text, a native
/// spelling's backslashes would read as escapes.
static std::optional<CompiledRule::Pattern> compile_pattern(std::string pattern,
                                                            llvm::StringRef anchor,
                                                            llvm::StringRef workspace_root) {
    // A substituted workspace root is path, never glob syntax: the wildcard
    // search starts after it, and the literal prefix it lands in is escaped.
    std::size_t search_from =
        llvm::StringRef(pattern).starts_with("${workspace}") ? workspace_root.size() : 0;
    substitute_workspace(pattern, workspace_root);
    llvm::StringRef ref(pattern);
    std::string text = pattern;
    std::string root = workspace_root.str();
    if(!ref.starts_with("**")) {
        auto wildcard = ref.find_first_of(R"(*?[{\)", search_from);
        auto cut = ref.rfind('/', wildcard == llvm::StringRef::npos ? ref.size() : wildcard);
        llvm::SmallString<256> dir;
        if(cut != llvm::StringRef::npos) {
            dir = ref.take_front(cut + 1);
        }
        if(!path::is_absolute(dir)) {
            llvm::SmallString<256> anchored(anchor);
            if(!dir.empty()) {
                path::append(anchored, dir);
            }
            dir = anchored;
        }
        path::remove_dots(dir, /*remove_dot_dot=*/true);
        root = std::string(dir);
        path::canonicalize(root);
        text = glob_escape(root);
        if(!text.ends_with('/')) {
            text += '/';
        }
        text += cut == llvm::StringRef::npos ? ref : ref.drop_front(cut + 1);
    }
    auto glob = kota::GlobPattern::create(text);
    if(!glob) {
        LOG_WARN("Invalid glob pattern in rule: {}: {}", pattern, glob.error().message);
        return std::nullopt;
    }
    return CompiledRule::Pattern{.glob = std::move(*glob), .root = std::move(root)};
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

    this->workspace_root = workspace_root.str();
    path::canonicalize(this->workspace_root);
    llvm::StringRef root = this->workspace_root;

    if(p.cache_dir.empty() && !root.empty()) {
        p.cache_dir = path::join(root, ".clice");
        p.cache_dir_defaulted = true;
    }
    if(p.logging_dir.empty() && !p.cache_dir.empty())
        p.logging_dir = path::join(p.cache_dir, "logs");

    // Variable substitution on string fields; a path still relative after
    // it came from initializationOptions (load() anchored a file's own)
    // and resolves against the workspace root.
    for(std::string* dir: std::initializer_list<std::string*>{&p.cache_dir, &p.logging_dir}) {
        substitute_workspace(*dir, root);
        if(!dir->empty() && !root.empty() && !path::is_absolute(*dir)) {
            *dir = path::join(root, *dir);
        }
        // Client-supplied dirs arrive in native spelling (backslashes, any
        // drive case); canonicalize so artifact-prefix checks against
        // pool-resolved paths hold.
        path::canonicalize(*dir);
    }

    auto anchored = [&](std::string value, llvm::StringRef anchor) {
        substitute_workspace(value, root);
        llvm::SmallString<256> full(value);
        if(!path::is_absolute(full) && !anchor.empty()) {
            full = anchor;
            path::append(full, value);
        }
        path::remove_dots(full, /*remove_dot_dot=*/true);
        std::string result(full);
        path::canonicalize(result);
        return result;
    };

    compiled_rules.clear();
    auto compile = [&](const ConfigRule& rule) -> std::optional<CompiledRule> {
        // A rule read from a file anchors at the file's directory, one from
        // initializationOptions at the workspace root.
        std::string anchor = rule.directory.empty() ? root.str() : std::string(rule.directory);
        path::canonicalize(anchor);
        CompiledRule compiled;
        for(auto& pattern: rule.patterns) {
            if(auto compiled_pattern = compile_pattern(pattern, anchor, root)) {
                compiled.patterns.push_back(std::move(*compiled_pattern));
            }
        }
        // Without a valid pattern the rule can match nothing; its declared
        // databases still load, since entries apply regardless of patterns.
        if(compiled.patterns.empty() && !rule.patterns.empty()) {
            LOG_WARN("Rule matches no file: all of its glob patterns failed to compile");
            compiled.unmatchable = true;
        }
        compiled.configuration = rule.configuration;
        for(auto& database: rule.compile_commands) {
            auto full = anchored(database, anchor);
            // The directory form is told from the file form by extension
            // everywhere else; only the filesystem knows a directory
            // spelled with a .json suffix.
            if(fs::is_directory(full)) {
                full = path::join(full, "compile_commands.json");
            }
            compiled.compile_commands.push_back(std::move(full));
        }
        // A string spelling is tokenized before `${workspace}` is
        // substituted, so a root with spaces stays one argument.
        if(auto* spelling = std::get_if<std::string>(&rule.default_command)) {
            compiled.default_command = tokenize_command(*spelling);
        } else {
            compiled.default_command = std::get<std::vector<std::string>>(rule.default_command);
        }
        for(auto& arg: compiled.default_command) {
            substitute_workspace(arg, root);
        }
        compiled.directory = std::move(anchor);
        compiled.append.assign(rule.append.begin(), rule.append.end());
        compiled.remove.assign(rule.remove.begin(), rule.remove.end());
        compiled.index = rule.index;
        return compiled;
    };
    for(auto& rule: rules) {
        if(auto compiled = compile(rule)) {
            compiled_rules.push_back(std::move(*compiled));
        }
    }
    auto tags = configurations();
    if(!tags.empty() && default_configuration.empty()) {
        LOG_WARN("Rules declare configurations {} but default_configuration is unset; using {}",
                 llvm::join(tags, ", "),
                 tags.front());
    } else if(!default_configuration.empty() &&
              !llvm::is_contained(tags, llvm::StringRef(default_configuration))) {
        LOG_WARN("default_configuration = {} names no rule's configuration", default_configuration);
    }
}

bool CompiledRule::has_default_command() const {
    return !default_command.empty();
}

bool CompiledRule::declares_sources() const {
    return !compile_commands.empty() || has_default_command();
}

bool CompiledRule::matches(llvm::StringRef path) const {
    if(unmatchable) {
        return false;
    }
    return patterns.empty() || std::ranges::any_of(patterns, [&](const Pattern& pattern) {
               return pattern.glob.match(path);
           });
}

llvm::SmallVector<const CompiledRule*> Config::matching_rules(llvm::StringRef path,
                                                              llvm::StringRef configuration) const {
    llvm::SmallVector<const CompiledRule*> result;
    for(auto& rule: compiled_rules) {
        if((rule.configuration.empty() || rule.configuration == configuration) &&
           rule.matches(path)) {
            result.push_back(&rule);
        }
    }
    return result;
}

llvm::SmallVector<llvm::StringRef> Config::configurations() const {
    llvm::SmallVector<llvm::StringRef> tags;
    for(auto& rule: compiled_rules) {
        if(!rule.configuration.empty() && !llvm::is_contained(tags, rule.configuration)) {
            tags.push_back(rule.configuration);
        }
    }
    return tags;
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
    auto directory = path::parent_path(path).str();
    for(auto& rule: config.rules) {
        rule.directory = directory;
    }
    for(std::string* dir: std::initializer_list<std::string*>{&config.project.cache_dir,
                                                              &config.project.logging_dir}) {
        substitute_workspace(*dir, workspace_root);
        if(!dir->empty() && !path::is_absolute(*dir)) {
            *dir = path::join(directory, *dir);
        }
    }
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
        for(auto name: config_file_names) {
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
/// the enum fields name their accepted values so editors flag a typo
/// that would silently fall back to the default.
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
