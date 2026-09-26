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
    std::size_t pos = 0;
    while((pos = value.find(path::workspace_anchor, pos)) != std::string::npos) {
        value.replace(pos, path::workspace_anchor.size(), workspace_root);
        pos += workspace_root.size();
    }
}

/// A path-valued setting as written: `${workspace}` substituted, a leading
/// `~` expanded to the home directory, a relative one anchored at `anchor`.
/// Nullopt for a relative one with no anchor to hold it (a rootless
/// project's setting).
static std::optional<Spelling> setting_path(std::string value,
                                            const Spelling& anchor,
                                            CanonicalRef workspace_root) {
    substitute_workspace(value, workspace_root);
    if(llvm::StringRef(value).starts_with("~")) {
        llvm::SmallString<256> expanded;
        llvm::sys::fs::expand_tilde(value, expanded);
        value = std::string(expanded);
    }
    if(path::is_absolute(value)) {
        return Spelling::absolute(value);
    }
    if(anchor.str().empty()) {
        return std::nullopt;
    }
    return Spelling(value, anchor);
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
/// the first wildcard segment is anchored (a relative one at `anchor`) and
/// resolved like every path the file table names — a symlinked directory
/// matches the files it holds — and the wildcard tail follows verbatim.
/// A `**`-led pattern matches anywhere and enumerates from the workspace.
static std::optional<CompiledRule::Pattern> compile_pattern(std::string pattern,
                                                            const Spelling& anchor,
                                                            CanonicalRef workspace_root) {
    // A substituted workspace root is path, never glob syntax: the wildcard
    // search starts after it, and the literal prefix it lands in is escaped.
    std::size_t search_from =
        llvm::StringRef(pattern).starts_with(path::workspace_anchor) ? workspace_root.size() : 0;
    substitute_workspace(pattern, workspace_root);
    llvm::StringRef ref(pattern);
    std::string text = pattern;
    CanonicalPath root = workspace_root;
    if(!ref.starts_with("**")) {
        auto wildcard = ref.find_first_of(R"(*?[{\)", search_from);
        auto cut = ref.rfind('/', wildcard == llvm::StringRef::npos ? ref.size() : wildcard);
        auto dir = cut == llvm::StringRef::npos ? llvm::StringRef() : ref.take_front(cut + 1);
        root =
            CanonicalPath(path::is_absolute(dir) ? Spelling::absolute(dir) : Spelling(dir, anchor));
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

void Config::finalize(CanonicalRef workspace_root) {
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

    this->workspace_root = workspace_root;
    CanonicalRef root = this->workspace_root;
    Spelling root_anchor = root.empty() ? Spelling() : Spelling(root);

    if(p.cache_dir.empty() && !root.empty()) {
        p.cache_dir = path::join(root, ".clice");
        p.cache_dir_defaulted = true;
    }
    if(p.logging_dir.empty() && !p.cache_dir.empty())
        p.logging_dir = path::join(p.cache_dir, "logs");

    // A path still relative here came from initializationOptions (load()
    // anchored a file's own) and resolves against the workspace root; the
    // file table's name for it is the one kept, whatever the user wrote.
    for(std::string* dir: std::initializer_list<std::string*>{&p.cache_dir, &p.logging_dir}) {
        if(dir->empty()) {
            continue;
        }
        auto spelled = setting_path(*dir, root_anchor, root);
        if(!spelled) {
            LOG_GUIDANCE("{} is relative in a project without a folder; ignored", *dir);
            dir->clear();
            continue;
        }
        *dir = CanonicalPath(*spelled).str();
    }

    compiled_rules.clear();
    auto compile = [&](const ConfigRule& rule) -> std::optional<CompiledRule> {
        // A rule read from a file anchors at the file's directory, one from
        // initializationOptions at the workspace root; a project without a
        // folder has nothing to anchor one at.
        auto anchor =
            rule.directory.empty() ? root_anchor : Spelling::absolute(std::string(rule.directory));
        if(anchor.str().empty()) {
            LOG_GUIDANCE(
                "Rules need a workspace folder to anchor their paths; a rule "
                "without one is ignored");
            return std::nullopt;
        }
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
            auto full = *setting_path(database, anchor, root);
            // The directory form is told from the file form by extension
            // everywhere else; only the filesystem knows a directory
            // spelled with a .json suffix.
            if(fs::is_directory(full.str())) {
                full = Spelling("compile_commands.json", full);
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
        for(auto* flags: {&compiled.append, &compiled.remove}) {
            for(auto& flag: *flags) {
                substitute_workspace(flag, root);
            }
        }
        compiled.index = rule.index;
        compiled.lint = rule.lint;
        compiled.format = rule.format;
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
    return !compile_commands.empty() || (has_default_command() && !unmatchable);
}

bool CompiledRule::matches(CanonicalRef path) const {
    if(unmatchable) {
        return false;
    }
    return patterns.empty() || std::ranges::any_of(patterns, [&](const Pattern& pattern) {
               return pattern.glob.match(llvm::StringRef(path));
           });
}

llvm::SmallVector<const CompiledRule*> Config::matching_rules(CanonicalRef path,
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
                                   CanonicalRef workspace_root,
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
    auto directory = Spelling::absolute(path).parent();
    for(auto& rule: config.rules) {
        rule.directory = directory.str();
    }
    for(std::string* dir: std::initializer_list<std::string*>{&config.project.cache_dir,
                                                              &config.project.logging_dir}) {
        if(!dir->empty()) {
            *dir = setting_path(*dir, directory, workspace_root)->str();
        }
    }
    if(finalized)
        config.finalize(workspace_root);
    LOG_INFO("Loaded config from {}", path);
    return config;
}

std::optional<Config> Config::load_from_json(llvm::StringRef json, CanonicalRef workspace_root) {
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

Config Config::load_from_workspace(CanonicalRef workspace_root,
                                   std::vector<ConfigIssue>* issues,
                                   std::string* loaded_path,
                                   bool finalized) {
    if(loaded_path)
        loaded_path->clear();

    auto config = [&] {
        bool found = false;
        if(!workspace_root.empty()) {
            for(auto name: config_file_names) {
                auto config_path = path::join(llvm::StringRef(workspace_root), name);
                if(!llvm::sys::fs::exists(config_path))
                    continue;
                found = true;
                if(loaded_path)
                    *loaded_path = config_path;
                if(auto config = load(config_path, workspace_root, issues, /*finalized=*/false))
                    return std::move(*config);
                // Present but malformed: fall through to defaults, but
                // surface the situation clearly so users know their config
                // wasn't applied.
                LOG_WARN("Falling back to default configuration because {} is invalid",
                         config_path);
            }
        }
        if(!found) {
            LOG_INFO("No clice.toml found in {}, using default configuration", workspace_root);
        }
        return Config();
    }();
    if(finalized) {
        config.finalize(workspace_root);
        config.keep_own_cache_dir();
    }
    return config;
}

/// Where a cache directory outside its workspace root records its owner.
constexpr static llvm::StringRef cache_owner_file = "owner";

std::string cache_dir_owner(llvm::StringRef cache_dir) {
    auto owner = fs::read(path::join(cache_dir, cache_owner_file));
    return owner ? llvm::StringRef(*owner).trim().str() : std::string();
}

/// A recorded owner that no longer exists (a moved or deleted checkout)
/// claims nothing.
static bool live_owner(llvm::StringRef owner, llvm::StringRef root) {
    return !owner.empty() && owner != root && llvm::sys::fs::is_directory(owner);
}

bool owned_elsewhere(llvm::StringRef cache_dir, CanonicalRef workspace_root) {
    return !path::under(CanonicalPath(Spelling::absolute(cache_dir)), workspace_root) &&
           live_owner(cache_dir_owner(cache_dir), workspace_root);
}

void claim_cache_dir(llvm::StringRef cache_dir, CanonicalRef root) {
    auto owner = cache_dir_owner(cache_dir);
    // One inside the root is the root's, whatever it records — a copied
    // checkout carries the original's record along.
    if(owner == root.str() || (!path::under(CanonicalPath(Spelling::absolute(cache_dir)), root) &&
                               live_owner(owner, root))) {
        return;
    }
    if(auto written = fs::write(path::join(cache_dir, cache_owner_file), root.str() + "\n");
       !written) {
        LOG_WARN("Cannot record the owner of cache directory {}: {}",
                 cache_dir,
                 written.error().message());
    }
}

void Config::keep_own_cache_dir() {
    auto& p = project;
    if(p.cache_dir.empty() || !owned_elsewhere(p.cache_dir, workspace_root)) {
        return;
    }
    auto own = path::join(workspace_root, ".clice");
    path::canonicalize(own);
    LOG_GUIDANCE("Cache directory {} serves the project at {}; {} keeps its cache in {}",
                 std::string(p.cache_dir),
                 cache_dir_owner(p.cache_dir),
                 workspace_root,
                 own);
    if(path::under(p.logging_dir, p.cache_dir)) {
        p.logging_dir = path::join(own, "logs");
    }
    p.cache_dir = std::move(own);
    p.cache_dir_defaulted = true;
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
