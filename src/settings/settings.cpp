module clice;

import :command.argument;
import :settings.configuration;
import :settings.settings;
import :support.logging;
import :support.process;
import :vfs.file_system;
import :vfs.path;

namespace clice {

/// What a setting's `${...}` names: the project root; `workspace_id`, a
/// name of the project root's own, its directory's name and a hash of its
/// path; or `env:NAME`, an environment variable. An unset variable, or a
/// word naming none, is an error naming it.
static std::expected<std::string, std::string> variable(llvm::StringRef name,
                                                        CanonicalRef project_root) {
    if(name == "workspace") {
        return project_root.str();
    }
    if(name == "workspace_id") {
        llvm::StringRef root = project_root;
        return std::format("{}-{:08x}",
                           path::filename(root),
                           static_cast<std::uint32_t>(llvm::xxh3_64bits(root)));
    }
    if(auto env = name; env.consume_front("env:")) {
        if(auto value = env_string(env)) {
            return std::move(*value);
        }
        return std::unexpected(std::format(
            "the environment variable {} is not set (a server an editor starts may not see "
            "the shell's environment; VS Code passes one through `clice.serverEnv`)",
            env));
    }
    return std::unexpected(std::format(
        "${{{}}} names nothing: write ${{workspace}}, ${{workspace_id}} or ${{env:NAME}}",
        name));
}

/// `value` with its variables substituted, each value escaped by `escape`
/// (a glob's, so that a path stays a path). `${workspace}` and
/// `${workspace_id}` stay as written in a project without a folder, where
/// they name nothing.
static std::expected<std::string, std::string>
    substitute(llvm::StringRef value,
               CanonicalRef project_root,
               llvm::function_ref<std::string(llvm::StringRef)> escape = {}) {
    std::string result;
    while(true) {
        auto open = value.find("${");
        auto close = open == llvm::StringRef::npos ? open : value.find('}', open);
        if(close == llvm::StringRef::npos) {
            result += value;
            return result;
        }
        result += value.take_front(open);
        auto name = value.slice(open + 2, close);
        if((name == "workspace" || name == "workspace_id") && project_root.empty()) {
            result += value.slice(open, close + 1);
        } else {
            auto replaced = variable(name, project_root);
            if(!replaced) {
                return std::unexpected(std::move(replaced.error()));
            }
            result += escape ? escape(*replaced) : *replaced;
        }
        value = value.drop_front(close + 1);
    }
}

/// A path-valued setting as written, its variables substituted: a leading
/// `~` expanded to the home directory, a relative one anchored at the
/// project root. Nullopt for a relative one in a project without a folder,
/// which has nothing to anchor it at.
static std::optional<Spelling> setting_path(std::string value, CanonicalRef project_root) {
    if(std::string expanded; !sys::expand_tilde(expanded, value)) {
        value = std::move(expanded);
    }
    if(path::is_absolute(value)) {
        return Spelling::absolute(value);
    }
    if(project_root.empty()) {
        return std::nullopt;
    }
    return Spelling(value, Spelling(project_root));
}

/// The physical cores this process may run on, clangd's default too: a
/// hyper-thread adds little to a compile, while every worker costs a TU's
/// memory.
static std::uint32_t physical_parallelism() {
    return std::min(sys::physical_cores(), sys::parallelism());
}

std::uint32_t auto_background_count() {
    // Settings is constructed on every worker request (the feature and build
    // params carry one); query the core count once, not per request.
    const static std::uint32_t count = std::max(physical_parallelism() / 2, 2u);
    return count;
}

std::uint32_t auto_max_count() {
    const static std::uint32_t count = physical_parallelism();
    return count;
}

std::uint32_t WorkersSettings::background_count() const {
    auto* count = std::get_if<std::uint32_t>(&background);
    return std::min(count ? *count : auto_background_count(), max_count());
}

std::uint32_t WorkersSettings::max_count() const {
    auto* count = std::get_if<std::uint32_t>(&max);
    return count ? *count : auto_max_count();
}

/// The glob `text`, the pattern `written` once its variables are
/// substituted.
static std::expected<kotatsu::GlobPattern, std::string> compile_glob(llvm::StringRef written,
                                                                     llvm::StringRef text) {
    // A backslash escapes the next character in a glob, so a Windows path
    // written with them compiles and matches nothing. A substituted path is
    // escaped with them, so they are looked for before.
    if(written.contains('\\')) {
        return std::unexpected(std::format(
            "'{}' holds a backslash: separate directories with `/` on every platform, and "
            "match a literal `*`, `?`, `[` or `{{` with a range such as `[*]`",
            written));
    }
    auto glob = kotatsu::GlobPattern::create(text);
    if(!glob) {
        return std::unexpected(
            std::format("invalid glob pattern '{}': {}", written, glob.error().message));
    }
    return std::move(*glob);
}

/// Compile one pattern against absolute paths: the literal directory before
/// the first wildcard segment is anchored at the project root and resolved
/// like every path the file table names — a symlinked directory matches the
/// files it holds — and the wildcard tail follows verbatim. A `**`-led
/// pattern matches anywhere and enumerates from the project root.
static std::expected<CompiledRule::Pattern, std::string>
    compile_pattern(llvm::StringRef written, CanonicalRef project_root) {
    auto pattern = substitute(written, project_root, [](llvm::StringRef value) {
        return kotatsu::GlobPattern::escape(value);
    });
    if(!pattern) {
        return std::unexpected(std::move(pattern.error()));
    }
    std::string text = *pattern;
    CanonicalPath root = project_root;
    if(!pattern->starts_with("**")) {
        auto [directory, rest] = kotatsu::GlobPattern::split_root(*pattern);
        root = CanonicalPath(Spelling(directory, Spelling(project_root)));
        text = kotatsu::GlobPattern::escape(root);
        if(!text.ends_with('/')) {
            text += '/';
        }
        text += rest;
    }
    auto glob = compile_glob(written, text);
    if(!glob) {
        return std::unexpected(std::move(glob.error()));
    }
    return CompiledRule::Pattern{.glob = std::move(*glob),
                                 .text = std::move(text),
                                 .root = std::move(root)};
}

std::vector<codec::rich_error> Settings::finalize(CanonicalRef project_root) {
    std::vector<codec::rich_error> problems;
    auto problem = [&](std::string message, std::vector<codec::rich_error::path_segment> path) {
        auto& error = problems.emplace_back(std::move(message));
        error.path = std::move(path);
    };

    // Zero workers or a zero limit is never a runnable setting: an error,
    // and the field's born-valid default meanwhile.
    WorkersSettings workers_defaults;
    if(workers.foreground == 0) {
        problem("a worker count must be at least 1", {"workers", "foreground"});
        workers.foreground = workers_defaults.foreground;
    }
    for(auto [name, count]: {
            std::pair{"background", &workers.background},
            {"max",        &workers.max       }
    }) {
        if(auto* number = std::get_if<std::uint32_t>(count); number && *number == 0) {
            problem(R"(a worker count must be at least 1, or "auto")", {"workers", name});
            *count = Automatic::Auto;
        }
    }
    auto reject_zero = [&](Limit& limit,
                           const Limit& fallback,
                           std::vector<codec::rich_error::path_segment> path) {
        if(auto* number = std::get_if<std::uint32_t>(&limit); number && *number == 0) {
            problem(R"(a limit must be at least 1; "unlimited" lifts it)", std::move(path));
            limit = fallback;
        }
    };
    reject_zero(references.limit, ReferencesSettings().limit, {"references", "limit"});
    reject_zero(code_completion.limit,
                feature::CodeCompletionOptions().limit,
                {"code_completion", "limit"});
    feature::InlayHintLengths lengths;
    reject_zero(inlay_hints.max_length.types,
                lengths.types,
                {"inlay_hints", "max_length", "types"});
    reject_zero(inlay_hints.max_length.default_arguments,
                lengths.default_arguments,
                {"inlay_hints", "max_length", "default_arguments"});
    reject_zero(inlay_hints.max_length.block_end,
                lengths.block_end,
                {"inlay_hints", "max_length", "block_end"});
    reject_zero(inlay_hints.max_length.endif,
                lengths.endif,
                {"inlay_hints", "max_length", "endif"});

    include_delimiters = {};
    auto compile_globs = [&](llvm::StringRef key,
                             llvm::ArrayRef<std::string> written,
                             std::vector<kotatsu::GlobPattern>& globs) {
        for(auto [element, pattern]: views::enumerate(written)) {
            auto glob = compile_glob(pattern, pattern);
            if(!glob) {
                problem(std::move(glob.error()),
                        {"code_completion", key.str(), static_cast<std::size_t>(element)});
                continue;
            }
            globs.push_back(std::move(*glob));
        }
    };
    compile_globs("quoted_includes", code_completion.quoted_includes, include_delimiters.quoted);
    compile_globs("angled_includes", code_completion.angled_includes, include_delimiters.angled);

    CanonicalRef root = project_root;

    if(cache_dir.empty() && !root.empty()) {
        cache_dir = path::join(root, ".clice");
        cache_dir_defaulted = true;
    }
    // The file table's name for a directory is the one kept, whatever the
    // user wrote.
    auto resolve_dir = [&](llvm::StringRef name, std::string& dir) {
        auto substituted = substitute(dir, root);
        if(!substituted) {
            problem(std::move(substituted.error()), {name.str()});
            dir.clear();
            return;
        }
        // A directory of the project's own, set for every project, is none
        // without a folder.
        if(root.empty() && llvm::StringRef(*substituted).contains("${workspace")) {
            dir.clear();
            return;
        }
        auto spelled = setting_path(std::move(*substituted), root);
        if(!spelled) {
            LOG_GUIDANCE("{} is relative in a project without a folder; ignored", dir);
            dir.clear();
            return;
        }
        dir = CanonicalPath(*spelled).str();
    };
    if(!cache_dir.empty()) {
        resolve_dir("cache_dir", cache_dir);
    }
    if(!logging_dir.empty()) {
        resolve_dir("logging_dir", logging_dir);
    } else if(!cache_dir.empty()) {
        logging_dir = path::join(cache_dir, "logs");
    }
    cache_identity =
        cache_dir.empty() ? CanonicalPath() : CanonicalPath(Spelling::absolute(cache_dir));

    compiled_rules.clear();
    if(!rules.empty() && root.empty()) {
        static std::once_flag reported;
        std::call_once(reported, [] {
            LOG_GUIDANCE(
                "Rules need a workspace folder to anchor their paths: a file outside every "
                "folder is served without them");
        });
    }
    for(auto [index, rule]:
        views::enumerate(root.empty() ? llvm::ArrayRef<Rule>() : llvm::ArrayRef(rules))) {
        auto at = [&](std::string key) {
            return std::vector<codec::rich_error::path_segment>{"rules",
                                                                static_cast<std::size_t>(index),
                                                                std::move(key)};
        };
        auto at_element = [&](std::string key, std::size_t element) {
            auto path = at(std::move(key));
            path.push_back(element);
            return path;
        };
        CompiledRule compiled;
        for(auto [element, pattern]: views::enumerate(rule.patterns)) {
            auto compiled_pattern = compile_pattern(pattern, root);
            if(!compiled_pattern) {
                problem(std::move(compiled_pattern.error()),
                        at_element("patterns", static_cast<std::size_t>(element)));
                continue;
            }
            compiled.patterns.push_back(std::move(*compiled_pattern));
        }
        // Without a valid pattern the rule can match nothing; its declared
        // databases still load, since entries apply regardless of patterns.
        compiled.unmatchable = compiled.patterns.empty() && !rule.patterns.empty();
        compiled.configuration = rule.configuration;
        for(auto [element, database]: views::enumerate(rule.compile_commands)) {
            auto substituted = substitute(database, root);
            if(!substituted) {
                problem(std::move(substituted.error()),
                        at_element("compile_commands", static_cast<std::size_t>(element)));
                continue;
            }
            auto full = *setting_path(std::move(*substituted), root);
            // The directory form is told from the file form by extension
            // everywhere else; only the filesystem knows a directory
            // spelled with a .json suffix.
            if(fs::is_directory(full)) {
                full = Spelling("compile_commands.json", full);
            }
            compiled.compile_commands.push_back(std::move(full));
        }
        compiled.resolve_symlinks = rule.resolve_symlinks;
        // A string spelling is tokenized before its variables are
        // substituted, so a value with spaces stays one argument.
        if(auto* spelling = std::get_if<std::string>(&rule.default_command)) {
            compiled.default_command = split_command(*spelling);
        } else {
            compiled.default_command = std::get<std::vector<std::string>>(rule.default_command);
        }
        auto expand = [&](std::vector<std::string>& args,
                          std::vector<codec::rich_error::path_segment> path) {
            for(auto& arg: args) {
                auto substituted = substitute(arg, root);
                if(!substituted) {
                    problem(std::move(substituted.error()), path);
                    args.clear();
                    return;
                }
                arg = std::move(*substituted);
            }
        };
        expand(compiled.default_command, at("default_command"));
        compiled.append = rule.append;
        compiled.remove = rule.remove;
        expand(compiled.append, at("append"));
        expand(compiled.remove, at("remove"));
        compiled.query_toolchain = rule.query_toolchain;
        compiled.background_index = rule.background_index;
        compiled.skip_function_body = rule.skip_function_body;
        compiled.index_function_body = rule.index_function_body;
        compiled.readonly = rule.readonly;
        compiled.self_contained = rule.self_contained;
        compiled.other_contexts = rule.other_contexts;
        compiled.lint = rule.lint;
        compiled.format = rule.format;
        compiled.layer = rule.layer;
        compiled_rules.push_back(std::move(compiled));
    }
    if(!root.empty() && !default_configuration.empty() && !declares(default_configuration)) {
        auto tags = configurations();
        problem(tags.empty() ? std::format("{} names a configuration, but no rule declares one",
                                           default_configuration)
                             : std::format("{} names no rule's configuration ({})",
                                           default_configuration,
                                           llvm::join(tags, ", ")),
                {"default_configuration"});
    }
    return problems;
}

bool CompiledRule::has_default_command() const {
    return !default_command.empty();
}

bool CompiledRule::declares_sources() const {
    return !compile_commands.empty() || (has_default_command() && !unmatchable);
}

bool CompiledRule::matches(const RulePath& path) const {
    if(unmatchable) {
        return false;
    }
    auto names = path.names();
    return patterns.empty() || ranges::any_of(patterns, [&](const Pattern& pattern) {
               return ranges::any_of(names, [&](llvm::StringRef name) {
                   return pattern.glob.match(name);
               });
           });
}

bool CompiledRule::active_under(llvm::StringRef configuration) const {
    return this->configuration.empty() || this->configuration == configuration;
}

llvm::SmallVector<llvm::StringRef> Settings::configurations() const {
    llvm::SmallVector<llvm::StringRef> tags;
    for(auto& rule: compiled_rules) {
        if(!rule.configuration.empty() && !ranges::contains(tags, rule.configuration)) {
            tags.push_back(rule.configuration);
        }
    }
    return tags;
}

bool Settings::declares(llvm::StringRef configuration) const {
    return ranges::contains(configurations(), configuration);
}

llvm::StringRef Settings::fallback_configuration() const {
    if(!default_configuration.empty()) {
        return default_configuration;
    }
    auto tags = configurations();
    return tags.empty() ? llvm::StringRef() : tags.front();
}

/// The end of the key or value below `node` that starts at `begin`, the
/// innermost one: an array of tables starts where its first table does.
static std::optional<toml::source_position> end_at(const toml::node& node,
                                                   const toml::source_position& begin) {
    auto at = [&](const toml::node& child) -> std::optional<toml::source_position> {
        if(auto end = end_at(child, begin)) {
            return end;
        }
        if(child.source().begin == begin) {
            return child.source().end;
        }
        return std::nullopt;
    };
    if(auto* table = node.as_table()) {
        for(auto& [key, value]: *table) {
            if(key.source().begin == begin) {
                return key.source().end;
            }
            if(auto end = at(value)) {
                return end;
            }
        }
    } else if(auto* array = node.as_array()) {
        for(auto& element: *array) {
            if(auto end = at(element)) {
                return end;
            }
        }
    }
    return std::nullopt;
}

/// The node of `table` at `path`, as a decode error or a finalize problem
/// names it; null when the document holds none there.
const static toml::node* node_at(const toml::table& table,
                                 llvm::ArrayRef<codec::rich_error::path_segment> path) {
    const toml::node* node = &table;
    for(auto& segment: path) {
        if(auto* key = std::get_if<std::string>(&segment)) {
            auto* within = node->as_table();
            node = within ? within->get(*key) : nullptr;
        } else {
            auto* within = node->as_array();
            node = within ? within->get(std::get<std::size_t>(segment)) : nullptr;
        }
        if(!node) {
            return nullptr;
        }
    }
    return node;
}

/// Whether the overlay sets the value at `path`: a problem there is the
/// overlay's.
static bool sets(const codec::dyn::Value& overlay,
                 llvm::ArrayRef<codec::rich_error::path_segment> path) {
    const codec::dyn::Value* value = &overlay;
    for(auto& segment: path) {
        if(auto* key = std::get_if<std::string>(&segment)) {
            auto* object = value->get_object();
            value = object ? object->find(*key) : nullptr;
        } else {
            auto* array = value->get_array();
            auto index = std::get<std::size_t>(segment);
            value = array && index < array->size() ? &(*array)[index] : nullptr;
        }
        if(!value) {
            return false;
        }
    }
    return true;
}

/// An issue of the file `path` at its error's position, spanning the key or
/// value of `parsed` that starts there: the error carries only where it
/// starts. An error with no position (a problem finalize found, an unknown
/// enumerator) spans the value its path names. A document that does not
/// parse has no such extent, its error marks a point.
static SettingsIssue make_issue(SettingsIssue::Severity severity,
                                llvm::StringRef path,
                                const codec::rich_error& error,
                                const toml::table* parsed) {
    SettingsIssue issue;
    issue.severity = severity;
    issue.file = path.str();
    // The position is the issue's own; the message leaves it out.
    auto described = error;
    described.location.reset();
    issue.message = described.to_string();
    if(!error.location) {
        if(auto* node = parsed ? node_at(*parsed, error.path) : nullptr) {
            auto& source = node->source();
            issue.line = source.begin.line;
            issue.column = source.begin.column;
            issue.end_line = source.end.line;
            issue.end_column = source.end.column;
        }
        return issue;
    }
    issue.line = issue.end_line = static_cast<std::uint32_t>(error.location->line);
    issue.column = issue.end_column = static_cast<std::uint32_t>(error.location->column);
    if(!parsed) {
        return issue;
    }
    if(auto end = end_at(*parsed,
                         {static_cast<toml::source_index>(issue.line),
                          static_cast<toml::source_index>(issue.column)})) {
        issue.end_line = end->line;
        issue.end_column = end->column;
    }
    return issue;
}

bool has_error(llvm::ArrayRef<SettingsIssue> issues) {
    return ranges::any_of(issues, [](const SettingsIssue& issue) {
        return issue.severity == SettingsIssue::Severity::Error;
    });
}

bool LoadedSettings::broken() const {
    return has_error(issues);
}

/// What became of a key of the layout before clice 1.0, said before the
/// key's path: where it moved, or why it is gone. Empty for a key that
/// never was.
static llvm::StringRef moved_key(llvm::StringRef section, llvm::StringRef key) {
    constexpr std::array<std::array<llvm::StringRef, 3>, 16> moved = {
        {
         {"project", "cache_dir", "moved to the top-level `cache_dir` in clice 1.0"},
         {"project", "logging_dir", "moved to the top-level `logging_dir` in clice 1.0"},
         {"project",
             "enable_indexing",
             "moved to a last pattern-less rule's `background_index = false` in clice 1.0"},
         {"project",
             "standard_library_instantiations",
             "moved to the rules' `index_function_body` in clice 1.0"},
         {"project",
             "index_library_bodies",
             "moved to the rules' `index_function_body` in clice 1.0"},
         {"project",
             "skip_header_bodies",
             "moved to the rules' `skip_function_body` in clice 1.0"},
         {"project", "readonly", "moved to the rules' `readonly` in clice 1.0"},
         {"project", "idle_timeout_ms", "moved to `workers.background_delay_ms` in clice 1.0"},
         {"project", "stateful_worker_count", "moved to `workers.foreground` in clice 1.0"},
         {"project", "stateless_worker_count", "moved to `workers.background` in clice 1.0"},
         {"project", "max_stateless_worker_count", "moved to `workers.max` in clice 1.0"},
         {"project",
             "min_stateless_worker_count",
             "gone in clice 1.0 (an idle server keeps one worker of its own accord)"},
         {"project",
             "max_resident_documents",
             "gone in clice 1.0 (how many open files stay compiled follows the machine's "
             "memory)"},
         {"project",
             "test_hooks",
             "moved to the environment variable CLICE_TEST_HOOKS in clice 1.0"},
         {"tracker",
             "workspace_poll_seconds",
             "moved to the top-level `poll_seconds` in clice 1.0"},
         {"rules", "index", "moved to `background_index` in clice 1.0"},
         }
    };
    auto it = ranges::find_if(moved, [&](const auto& entry) {
        return entry[0] == section && entry[1] == key;
    });
    return it == moved.end() ? llvm::StringRef() : (*it)[2];
}

/// The problems an unknown key stands for (an entry of codec::UnknownFields:
/// "unknown field 'key'" at the object holding it): an error at each key of
/// the layout before clice 1.0, naming where it went — every key of a
/// removed section, which `keys` lists — else a warning.
static void
    report_unknown(const codec::rich_error& entry,
                   llvm::function_ref<std::vector<std::string>(llvm::StringRef section)> keys,
                   llvm::function_ref<void(SettingsIssue::Severity, codec::rich_error)> report) {
    using Severity = SettingsIssue::Severity;
    llvm::StringRef key = entry.message;
    if(!key.consume_front("unknown field '") || !key.consume_back("'")) {
        report(Severity::Warning, entry);
        return;
    }
    auto& path = entry.path;
    if(path.empty() && (key == "project" || key == "tracker")) {
        auto inners = keys(key);
        if(inners.empty()) {
            codec::rich_error error("gone in clice 1.0");
            error.path = {key.str()};
            report(Severity::Error, std::move(error));
        }
        for(auto& inner: inners) {
            auto now = moved_key(key, inner);
            codec::rich_error error(now.empty() ? "gone in clice 1.0" : now.str());
            error.path = {key.str(), inner};
            report(Severity::Error, std::move(error));
        }
        return;
    }
    auto* first = path.empty() ? nullptr : std::get_if<std::string>(&path.front());
    if(first && *first == "rules" && path.size() == 2) {
        if(auto now = moved_key("rules", key); !now.empty()) {
            codec::rich_error error(now.str());
            error.path = path;
            error.path.emplace_back(key.str());
            report(Severity::Error, std::move(error));
            return;
        }
    }
    report(Severity::Warning, entry);
}

/// Whether a rule writes a field it may leave out.
static bool written(const auto& field) {
    if constexpr(requires { field.has_value(); }) {
        return field.has_value();
    } else {
        return !field.empty();
    }
}

/// The index of `rules[index]` among the rules of its layer, which stand
/// together: its index in the file or the settings it is written in.
static std::size_t index_in_layer(const auto& rules, std::size_t index) {
    auto first = index;
    while(first > 0 && rules[first - 1].layer == rules[index].layer) {
        first -= 1;
    }
    return index - first;
}

/// Report each field a rule writes that an earlier rule of its layer for
/// every file, active whenever it is, writes too: the earlier one decides
/// every file. A higher layer deciding over a lower one is what it is for.
static void report_shadowed(llvm::ArrayRef<CompiledRule> rules,
                            llvm::function_ref<void(codec::rich_error)> report) {
    for(auto [later, rule]: views::enumerate(rules)) {
        auto check = [&](llvm::StringRef name, auto field) {
            if(!written(rule.*field)) {
                return;
            }
            for(auto [earlier, other]:
                views::enumerate(rules.take_front(static_cast<std::size_t>(later)))) {
                if(other.layer == rule.layer && other.patterns.empty() && !other.unmatchable &&
                   other.active_under(rule.configuration) && written(other.*field)) {
                    codec::rich_error error(std::format(
                        "never applies: rules[{}], a rule for every file, sets {} first; a rule "
                        "for some files goes before the rule for all",
                        index_in_layer(rules, static_cast<std::size_t>(earlier)),
                        name));
                    error.path = {"rules", static_cast<std::size_t>(later), name.str()};
                    report(std::move(error));
                    return;
                }
            }
        };
        check("default_command", &CompiledRule::default_command);
        check("query_toolchain", &CompiledRule::query_toolchain);
        check("background_index", &CompiledRule::background_index);
        check("skip_function_body", &CompiledRule::skip_function_body);
        check("index_function_body", &CompiledRule::index_function_body);
        check("readonly", &CompiledRule::readonly);
        check("self_contained", &CompiledRule::self_contained);
        check("other_contexts", &CompiledRule::other_contexts);
        check("lint", &CompiledRule::lint);
        check("format", &CompiledRule::format);
    }
}

std::string user_settings_file() {
    std::string dir;
    if(auto xdg = env_string("XDG_CONFIG_HOME"); xdg && path::is_absolute(*xdg)) {
        dir = std::move(*xdg);
    } else {
#ifdef _WIN32
        if(sys::config_dir(dir)) {
            return {};
        }
#else
        if(sys::home_dir(dir)) {
            return {};
        }
        dir = path::join(dir, ".config");
#endif
    }
    auto file = path::join(dir, "clice", "config.toml");
    path::canonicalize(file);
    return file;
}

namespace {

/// A configuration file a project's settings decoded.
struct DecodedFile {
    SettingsLayer layer;
    std::string path;
    toml::table document;
};

using SettingPath = std::vector<codec::rich_error::path_segment>;

/// The values of `actual` that `defaults` does not have, each at its key's
/// path, as JSON.
void differing(simdjson::dom::element actual,
               simdjson::dom::element defaults,
               std::vector<std::string>& path,
               std::vector<std::pair<std::vector<std::string>, std::string>>& out) {
    simdjson::dom::object fields, default_fields;
    if(!actual.get_object().get(fields) && !defaults.get_object().get(default_fields)) {
        for(auto [key, value]: fields) {
            path.emplace_back(key);
            simdjson::dom::element default_value;
            if(default_fields[key].get(default_value)) {
                out.emplace_back(path, simdjson::minify(value));
            } else {
                differing(value, default_value, path, out);
            }
            path.pop_back();
        }
        return;
    }
    auto text = simdjson::minify(actual);
    if(text != simdjson::minify(defaults)) {
        out.emplace_back(path, std::move(text));
    }
}

/// The settings of `settings` that are not their defaults for a project at
/// `root`, the rules aside, each with where `origin` says it is set.
std::vector<ChangedSetting>
    changed_settings(const Settings& settings,
                     CanonicalRef root,
                     llvm::function_ref<std::string(const SettingPath&)> origin) {
    std::vector<ChangedSetting> changed;
    // Against the defaults as the project's root derives them: its cache
    // and logging directories are no setting of the user's.
    Settings unset;
    unset.finalize(root);
    auto actual = json::to_string(settings);
    auto defaults = json::to_string(unset);
    simdjson::dom::parser actual_parser, defaults_parser;
    simdjson::dom::element actual_root, defaults_root;
    if(!actual || !defaults || actual_parser.parse(*actual).get(actual_root) ||
       defaults_parser.parse(*defaults).get(defaults_root)) {
        return changed;
    }
    std::vector<std::string> path;
    std::vector<std::pair<std::vector<std::string>, std::string>> found;
    differing(actual_root, defaults_root, path, found);
    for(auto& [keys, value]: found) {
        if(keys.front() == "rules") {
            continue;
        }
        changed.push_back({
            .key = llvm::join(keys, "."),
            .value = std::move(value),
            .origin = origin(SettingPath(keys.begin(), keys.end())),
        });
    }
    return changed;
}

}  // namespace

LoadedSettings load_settings(CanonicalRef root, const LoadSettingsOptions& options) {
    using Severity = SettingsIssue::Severity;
    LoadedSettings loaded;
    auto& issues = loaded.issues;
    if(!options.user_file.empty() && fs::exists(options.user_file)) {
        loaded.files.push_back({.layer = SettingsLayer::User, .path = options.user_file.str()});
    }
    if(!root.empty()) {
        for(auto name: settings_file_names) {
            if(auto path = path::join(root, name); fs::exists(path)) {
                loaded.files.push_back({.layer = SettingsLayer::Project, .path = std::move(path)});
                break;
            }
        }
        if(auto path = path::join(root, local_settings_file_name); fs::exists(path)) {
            loaded.files.push_back({.layer = SettingsLayer::Local, .path = std::move(path)});
        }
    }

    Settings layered;
    std::vector<DecodedFile> decoded_files;
    // What the project's own files name, where a cache directory set for
    // every project falls back to when another project has it.
    std::string own_cache_dir;
    for(auto& file: loaded.files) {
        auto& path = file.path;
        auto content = vfs::read_text(path);
        if(!content) {
            issues.push_back(
                {.severity = Severity::Error,
                 .file = path,
                 .message = std::format("cannot read it: {}", content.error().code.message())});
            continue;
        }
        auto table = codec::toml::parse_table(*content);
        if(!table) {
            issues.push_back(make_issue(Severity::Error, path, table.error(), nullptr));
            continue;
        }
        auto decoded = layered;
        codec::UnknownFields unknown;
        auto result = [&] {
            codec::scoped_context<codec::UnknownFields> collecting(unknown);
            return codec::toml::from_toml(*table, decoded);
        }();
        if(!result) {
            issues.push_back(make_issue(Severity::Error, path, result.error(), &*table));
            continue;
        }
        std::vector<SettingsIssue> found;
        for(auto& entry: unknown.entries) {
            report_unknown(
                entry,
                [&](llvm::StringRef section) {
                    std::vector<std::string> keys;
                    if(auto* node = table->get(section); node && node->as_table()) {
                        for(auto& [key, _]: *node->as_table()) {
                            keys.emplace_back(key.str());
                        }
                    }
                    return keys;
                },
                [&](Severity severity, codec::rich_error error) {
                    found.push_back(make_issue(severity, path, error, &*table));
                });
        }
        // A table yields its keys in name order; the problems read in the
        // file's.
        ranges::stable_sort(found, {}, [](const SettingsIssue& issue) {
            return std::pair(issue.line, issue.column);
        });
        issues.insert(issues.end(),
                      std::make_move_iterator(found.begin()),
                      std::make_move_iterator(found.end()));
        // The file's rules go before those of the files below it: of the
        // rules matching a file, the first to set a field decides it.
        if(table->contains("rules")) {
            for(auto& rule: decoded.rules) {
                rule.layer = file.layer;
            }
            decoded.rules.insert(decoded.rules.end(), layered.rules.begin(), layered.rules.end());
        }
        if(file.layer != SettingsLayer::User && table->contains("cache_dir")) {
            own_cache_dir = decoded.cache_dir;
        }
        layered = std::move(decoded);
        decoded_files.push_back({.layer = file.layer, .path = path, .document = std::move(*table)});
    }

    if(options.overlay) {
        auto overlaid = layered;
        codec::UnknownFields unknown;
        auto result = [&] {
            codec::scoped_context<codec::UnknownFields> collecting(unknown);
            return codec::dyn::from_dyn(*options.overlay, overlaid);
        }();
        auto issue = [&](Severity severity, const codec::rich_error& error) {
            issues.push_back(
                {.severity = severity,
                 .message = std::format("{}: {}", options.overlay_name, error.to_string())});
        };
        if(!result) {
            issue(Severity::Error, result.error());
        } else {
            for(auto& entry: unknown.entries) {
                report_unknown(
                    entry,
                    [&](llvm::StringRef name) {
                        std::vector<std::string> keys;
                        auto* object = options.overlay->get_object();
                        auto* section = object ? object->find(name) : nullptr;
                        if(auto* fields = section ? section->get_object() : nullptr) {
                            for(auto& [key, _]: *fields) {
                                keys.emplace_back(key);
                            }
                        }
                        return keys;
                    },
                    issue);
            }
            if(sets(*options.overlay, {"rules"})) {
                for(auto& rule: overlaid.rules) {
                    rule.layer = SettingsLayer::Editor;
                }
            }
            layered = std::move(overlaid);
        }
    }

    // The highest file setting the value at `path`; null for none.
    auto setting = [&](const SettingPath& path) -> const DecodedFile* {
        for(auto& file: views::reverse(decoded_files)) {
            if(node_at(file.document, path)) {
                return &file;
            }
        }
        return nullptr;
    };
    // A problem at `error.path` of the merged settings, where it is written:
    // a rule's in its layer, at its index there; any other value's in the
    // highest layer setting it.
    auto report = [&](Severity severity, codec::rich_error error) {
        auto& path = error.path;
        auto* key = std::get_if<std::string>(&path.front());
        bool overlay = false;
        const DecodedFile* file = nullptr;
        if(key && *key == "rules") {
            auto index = std::get<std::size_t>(path[1]);
            auto layer = layered.rules[index].layer;
            path[1] = index_in_layer(layered.rules, index);
            overlay = layer == SettingsLayer::Editor;
            if(!overlay) {
                file = &*ranges::find(decoded_files, layer, &DecodedFile::layer);
            }
        } else if(options.overlay && sets(*options.overlay, path)) {
            overlay = true;
        } else {
            file = setting(path);
        }
        if(overlay) {
            issues.push_back(
                {.severity = severity,
                 .message = std::format("{}: {}", options.overlay_name, error.to_string())});
            return;
        }
        issues.push_back(make_issue(severity,
                                    file ? llvm::StringRef(file->path) : llvm::StringRef(),
                                    error,
                                    file ? &file->document : nullptr));
    };

    // A cache directory serves one project (see owned_elsewhere), and one at
    // a time: its writer lock is the process's, so a second project of the
    // server would take it too. The merged configuration's falls back to the
    // project's files' own, then the default; with those taken too the
    // project runs nothing.
    loaded.settings = layered;
    auto problems = loaded.settings.finalize(root);
    auto& cache_dir = loaded.settings.cache_dir;
    const CanonicalPath& identity = loaded.settings.cache_identity;
    auto taken_here = [&] {
        return ranges::contains(options.taken_cache_dirs, identity);
    };
    auto taken = [&] {
        return !cache_dir.empty() && (taken_here() || owned_elsewhere(cache_dir, root));
    };
    bool own_cache = false;
    if(!root.empty() && taken()) {
        std::string requested = cache_dir;
        auto reason = taken_here()
                          ? std::string("already serves another project")
                          : std::format("serves the project at {}", cache_dir_owner(requested));
        // One the user's file or the editor sets is every project's.
        auto* writer = setting({"cache_dir"});
        bool shared = (options.overlay && sets(*options.overlay, {"cache_dir"})) ||
                      (writer && writer->layer == SettingsLayer::User);
        for(auto& fallback: {own_cache_dir, std::string()}) {
            loaded.settings = layered;
            loaded.settings.cache_dir = fallback;
            problems = loaded.settings.finalize(root);
            own_cache = !fallback.empty();
            if(!taken()) {
                break;
            }
        }
        auto shown = options.root_name.empty() ? llvm::StringRef(root) : options.root_name;
        if(taken()) {
            cache_dir.clear();
            loaded.settings.cache_dir_defaulted = false;
            loaded.settings.cache_identity = {};
            issues.push_back({
                .severity = Severity::Error,
                .message = std::format(
                    "Cache directory {} {}, and the other ones {} can use are taken too; set "
                    "`cache_dir` in its clice.toml to a directory of its own",
                    requested,
                    reason,
                    shown),
            });
        } else {
            LOG_GUIDANCE("Cache directory {} {}; {} keeps its cache in {}{}",
                         requested,
                         reason,
                         shown,
                         cache_dir,
                         shared ? " (a cache_dir set for every project gives each a directory "
                                  "of its own with ${workspace_id})"
                                : "");
        }
    }
    for(auto& problem: problems) {
        report(Severity::Error, std::move(problem));
    }

    for(auto [index, rule]: views::enumerate(loaded.settings.compiled_rules)) {
        auto at = index_in_layer(loaded.settings.compiled_rules, static_cast<std::size_t>(index));
        if(rule.layer == SettingsLayer::Editor) {
            rule.origin = std::format("{} rules[{}]", options.overlay_name, at);
            continue;
        }
        auto& file = *ranges::find(decoded_files, rule.layer, &DecodedFile::layer);
        if(auto* node = node_at(file.document, {"rules", at})) {
            rule.origin = std::format("{}:{}", file.path, node->source().begin.line);
        }
    }
    report_shadowed(loaded.settings.compiled_rules, [&](codec::rich_error warning) {
        report(Severity::Warning, std::move(warning));
    });

    loaded.changed = changed_settings(loaded.settings, root, [&](const SettingPath& path) {
        // A cache directory that fell back is the project's files' own.
        bool own = own_cache && path == SettingPath{"cache_dir"};
        if(!own && options.overlay && sets(*options.overlay, path)) {
            return options.overlay_name.str();
        }
        auto* file = setting(path);
        return file ? std::format("{}:{}",
                                  file->path,
                                  node_at(file->document, path)->source().begin.line)
                    : std::string();
    });
    loaded.configuration =
        choose_configuration(loaded.settings, options.configuration, options.strict, issues);
    return loaded;
}

/// Where a cache directory outside its project root records its owner.
constexpr static llvm::StringRef cache_owner_file = "owner";

std::string cache_dir_owner(llvm::StringRef cache_dir) {
    std::string owner;
    if(fs::read_file(owner, path::join(cache_dir, cache_owner_file))) {
        return {};
    }
    return llvm::StringRef(owner).trim().str();
}

/// Whether `owner`, a recorded owner as its project spelled it, is another
/// project than `root` and still exists: a moved or deleted checkout owns
/// nothing.
static bool live_owner(llvm::StringRef owner, CanonicalRef root) {
    return !owner.empty() && fs::is_directory(owner) &&
           CanonicalPath(Spelling::absolute(owner)) != root;
}

bool owned_elsewhere(llvm::StringRef cache_dir, CanonicalRef project_root) {
    return !path::is_within(CanonicalPath(Spelling::absolute(cache_dir)), project_root) &&
           live_owner(cache_dir_owner(cache_dir), project_root);
}

void set_cache_dir_owner(llvm::StringRef cache_dir, CanonicalRef root, llvm::StringRef shown) {
    // One inside the root is the root's, whatever it records — a copied
    // checkout carries the original's record along.
    if(cache_dir_owner(cache_dir) == shown || owned_elsewhere(cache_dir, root)) {
        return;
    }
    if(auto error = fs::write_file(path::join(cache_dir, cache_owner_file), shown.str() + "\n")) {
        LOG_WARN("Cannot record the owner of cache directory {}: {}", cache_dir, error.message());
    }
}

std::expected<std::string, std::string> Settings::json_schema() {
    // `additionalProperties: false` throughout: a key load() warns about as
    // unknown is one the schema rejects — the same typo is reported both ways.
    auto schema = json::schema_string<Settings>({.deny_unknown_fields = true, .pretty = true});
    if(!schema) {
        return std::unexpected(schema.error().message);
    }
    return std::move(*schema);
}

}  // namespace clice
