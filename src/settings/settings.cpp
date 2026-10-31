module clice;

import :settings.settings;
import :settings.configuration;
import :support.logging;
import :support.shell;
import :vfs.file_system;
import :vfs.path;

namespace clice {

/// Replace all occurrences of ${workspace} with the project root.
/// No-op when project_root is empty, to avoid producing paths like "/cache"
/// from "${workspace}/cache".
static void substitute_workspace(std::string& value, llvm::StringRef project_root) {
    if(project_root.empty())
        return;
    std::size_t pos = 0;
    while((pos = value.find(path::workspace_anchor, pos)) != std::string::npos) {
        value.replace(pos, path::workspace_anchor.size(), project_root);
        pos += project_root.size();
    }
}

/// A path-valued setting as written: `${workspace}` substituted, a leading
/// `~` expanded to the home directory, a relative one anchored at `anchor`.
/// Nullopt for a relative one with no anchor to hold it (a rootless
/// project's setting).
static std::optional<Spelling> setting_path(std::string value,
                                            const Spelling& anchor,
                                            CanonicalRef project_root) {
    substitute_workspace(value, project_root);
    if(std::string expanded; !sys::expand_tilde(expanded, value)) {
        value = std::move(expanded);
    }
    if(path::is_absolute(value)) {
        return Spelling::absolute(value);
    }
    if(anchor.empty()) {
        return std::nullopt;
    }
    return Spelling(value, anchor);
}

/// The physical cores this process may run on, clangd's default too: a
/// hyper-thread adds little to a compile, while every worker costs a TU's
/// memory.
static std::uint32_t physical_parallelism() {
    auto logical = sys::parallelism();
    auto physical = llvm::get_physical_cores();
    return physical > 0 ? std::min(static_cast<std::uint32_t>(physical), logical) : logical;
}

std::uint32_t default_stateless_worker_count() {
    // Settings is constructed on every worker request (the feature and build
    // params carry one); query the core count once, not per request.
    const static std::uint32_t count = std::max(physical_parallelism() / 2, 2u);
    return count;
}

std::uint32_t default_max_stateless_worker_count() {
    const static std::uint32_t count = physical_parallelism();
    return count;
}

/// Compile one pattern against absolute paths: the literal directory before
/// the first wildcard segment is anchored (a relative one at `anchor`) and
/// resolved like every path the file table names — a symlinked directory
/// matches the files it holds — and the wildcard tail follows verbatim.
/// A `**`-led pattern matches anywhere and enumerates from the project root.
static std::expected<CompiledRule::Pattern, std::string>
    compile_pattern(std::string pattern, const Spelling& anchor, CanonicalRef project_root) {
    // A substituted project root is path, never glob syntax.
    substitute_workspace(pattern, kotatsu::GlobPattern::escape(project_root));
    std::string text = pattern;
    CanonicalPath root = project_root;
    if(!pattern.starts_with("**")) {
        auto [directory, rest] = kotatsu::GlobPattern::split_root(pattern);
        root = CanonicalPath(Spelling(directory, anchor));
        text = kotatsu::GlobPattern::escape(root);
        if(!text.ends_with('/')) {
            text += '/';
        }
        text += rest;
    }
    auto glob = kotatsu::GlobPattern::create(text);
    if(!glob) {
        return std::unexpected(
            std::format("invalid glob pattern '{}': {}", pattern, glob.error().message));
    }
    return CompiledRule::Pattern{.glob = std::move(*glob), .root = std::move(root)};
}

std::vector<codec::rich_error> Settings::finalize(CanonicalRef project_root) {
    auto& p = project;
    std::vector<codec::rich_error> problems;
    auto problem = [&](std::string message, std::vector<codec::rich_error::path_segment> path) {
        auto& error = problems.emplace_back(std::move(message));
        error.path = std::move(path);
    };

    // Zero workers is never a runnable configuration: an error, and the
    // field's born-valid default meanwhile.
    ProjectSettings defaults;
    auto reject_zero = [&](auto& field, const auto& fallback, llvm::StringRef name) {
        if(field.value == 0) {
            problem("a worker count must be at least 1", {"project", name.str()});
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

    CanonicalRef root = project_root;
    Spelling root_anchor(root);

    if(p.cache_dir.empty() && !root.empty()) {
        p.cache_dir = path::join(root, ".clice");
        p.cache_dir_defaulted = true;
    }
    if(p.logging_dir.empty() && !p.cache_dir.empty())
        p.logging_dir = path::join(p.cache_dir, "logs");

    // A path still relative here came from initializationOptions (load()
    // anchored a file's own) and resolves against the project root; the
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
    auto compile = [&](const Rule& rule) -> std::optional<CompiledRule> {
        // A rule read from a file anchors at the file's directory, one from
        // initializationOptions at the project root; a project without a
        // folder has nothing to anchor one at.
        auto anchor =
            rule.directory.empty() ? root_anchor : Spelling::absolute(std::string(rule.directory));
        if(anchor.empty()) {
            LOG_GUIDANCE(
                "Rules need a workspace folder to anchor their paths; a rule "
                "without one is ignored");
            return std::nullopt;
        }
        CompiledRule compiled;
        for(auto [index, pattern]: views::enumerate(rule.patterns)) {
            auto compiled_pattern = compile_pattern(pattern, anchor, root);
            if(!compiled_pattern) {
                problem(std::move(compiled_pattern.error()),
                        {"rules",
                         static_cast<std::size_t>(&rule - rules.data()),
                         "patterns",
                         static_cast<std::size_t>(index)});
                continue;
            }
            compiled.patterns.push_back(std::move(*compiled_pattern));
        }
        // Without a valid pattern the rule can match nothing; its declared
        // databases still load, since entries apply regardless of patterns.
        compiled.unmatchable = compiled.patterns.empty() && !rule.patterns.empty();
        compiled.configuration = rule.configuration;
        for(auto& database: rule.compile_commands) {
            auto full = *setting_path(database, anchor, root);
            // The directory form is told from the file form by extension
            // everywhere else; only the filesystem knows a directory
            // spelled with a .json suffix.
            if(fs::is_directory(full)) {
                full = Spelling("compile_commands.json", full);
            }
            compiled.compile_commands.push_back(std::move(full));
        }
        compiled.resolve_symlinks = rule.resolve_symlinks;
        // A string spelling is tokenized before `${workspace}` is
        // substituted, so a root with spaces stays one argument.
        if(auto* spelling = std::get_if<std::string>(&rule.default_command)) {
            llvm::BumpPtrAllocator allocator;
            llvm::StringSaver saver(allocator);
            auto arguments = tokenize_command(*spelling, saver);
            compiled.default_command.assign(arguments.begin(), arguments.end());
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
        compiled.query_toolchain = rule.query_toolchain;
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
    if(!default_configuration.empty() && !declares(default_configuration)) {
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

bool CompiledRule::matches(CanonicalRef path) const {
    if(unmatchable) {
        return false;
    }
    return patterns.empty() || ranges::any_of(patterns, [&](const Pattern& pattern) {
               return pattern.glob.match(llvm::StringRef(path));
           });
}

bool CompiledRule::active_under(llvm::StringRef configuration) const {
    return this->configuration.empty() || this->configuration == configuration;
}

llvm::SmallVector<const CompiledRule*>
    Settings::matching_rules(CanonicalRef path, llvm::StringRef configuration) const {
    llvm::SmallVector<const CompiledRule*> result;
    for(auto& rule: compiled_rules) {
        if(rule.active_under(configuration) && rule.matches(path)) {
            result.push_back(&rule);
        }
    }
    return result;
}

llvm::SmallVector<Spelling> Settings::declared_databases(llvm::StringRef configuration) const {
    llvm::SmallVector<Spelling> result;
    for(auto& rule: compiled_rules) {
        if(!rule.active_under(configuration)) {
            continue;
        }
        for(auto& database: rule.compile_commands) {
            if(!ranges::contains(result, database)) {
                result.push_back(database);
            }
        }
    }
    return result;
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

/// Codec config the published schema is generated under, for its
/// `additionalProperties: false`: a key load() warns about as unknown is
/// one the schema rejects — the same typo is reported both ways.
struct DenyUnknownKeys {
    constexpr static bool deny_unknown_fields = true;
};

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

LoadedSettings load_settings(CanonicalRef root, const LoadSettingsOptions& options) {
    using Severity = SettingsIssue::Severity;
    LoadedSettings loaded;
    auto& issues = loaded.issues;
    Settings layered;
    std::optional<toml::table> parsed;
    for(auto name: root.empty() ? llvm::ArrayRef<llvm::StringRef>() : settings_file_names) {
        auto path = path::join(root, name);
        if(!fs::exists(path)) {
            continue;
        }
        loaded.file = path;
        auto content = vfs::read_text(path);
        if(!content) {
            issues.push_back(
                {.severity = Severity::Error,
                 .file = path,
                 .message = std::format("cannot read it: {}", content.error().message())});
            break;
        }
        auto table = codec::toml::parse_table(*content);
        if(!table) {
            issues.push_back(make_issue(Severity::Error, path, table.error(), nullptr));
            break;
        }
        parsed = std::move(*table);
        Settings decoded;
        codec::UnknownFields unknown;
        auto result = [&] {
            codec::scoped_context<codec::UnknownFields> collecting(unknown);
            return codec::toml::from_toml(*parsed, decoded);
        }();
        if(!result) {
            issues.push_back(make_issue(Severity::Error, path, result.error(), &*parsed));
            break;
        }
        for(auto& entry: unknown.entries) {
            issues.push_back(make_issue(Severity::Warning, path, entry, &*parsed));
        }
        // The file's relative paths and patterns anchor at its directory:
        // every rule records it, so the overlay keeps its own anchor.
        auto directory = Spelling::absolute(path).parent();
        for(auto& rule: decoded.rules) {
            rule.directory = directory.str();
        }
        for(std::string* dir: std::initializer_list<std::string*>{&decoded.project.cache_dir,
                                                                  &decoded.project.logging_dir}) {
            if(!dir->empty()) {
                *dir = std::move(*setting_path(*dir, directory, root)).str();
            }
        }
        layered = std::move(decoded);
        break;
    }
    std::string file_cache_dir = layered.project.cache_dir;

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
                issue(Severity::Warning, entry);
            }
            layered = std::move(overlaid);
        }
    }

    // A cache directory serves one project (see owned_elsewhere), and one at
    // a time: its writer lock is the process's, so a second project of the
    // server would take it too. The merged configuration's falls back to the
    // file's own, then the default, then none.
    loaded.settings = layered;
    auto problems = loaded.settings.finalize(root);
    auto& cache_dir = loaded.settings.project.cache_dir;
    auto taken_here = [&] {
        return ranges::contains(options.taken_cache_dirs, loaded.settings.project.cache_identity());
    };
    auto taken = [&] {
        return !cache_dir.empty() && (taken_here() || owned_elsewhere(cache_dir, root));
    };
    if(!root.empty() && taken()) {
        std::string requested = cache_dir;
        auto reason = taken_here()
                          ? std::string("already serves another project")
                          : std::format("serves the project at {}", cache_dir_owner(requested));
        for(auto& fallback: {file_cache_dir, std::string()}) {
            loaded.settings = layered;
            loaded.settings.project.cache_dir = fallback;
            problems = loaded.settings.finalize(root);
            if(!taken()) {
                break;
            }
        }
        if(taken()) {
            cache_dir.clear();
            loaded.settings.project.cache_dir_defaulted = false;
        }
        LOG_GUIDANCE("Cache directory {} {}; {} {}",
                     requested,
                     reason,
                     root,
                     cache_dir.empty() ? std::string("keeps no cache")
                                       : std::format("keeps its cache in {}", cache_dir));
    }
    for(auto& problem: problems) {
        if(options.overlay && sets(*options.overlay, problem.path)) {
            issues.push_back(
                {.severity = Severity::Error,
                 .message = std::format("{}: {}", options.overlay_name, problem.to_string())});
        } else {
            issues.push_back(
                make_issue(Severity::Error, loaded.file, problem, parsed ? &*parsed : nullptr));
        }
    }

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

/// A recorded owner that no longer exists (a moved or deleted checkout)
/// owns nothing.
static bool live_owner(llvm::StringRef owner, llvm::StringRef root) {
    return !owner.empty() && owner != root && fs::is_directory(owner);
}

bool owned_elsewhere(llvm::StringRef cache_dir, CanonicalRef project_root) {
    return !path::is_within(CanonicalPath(Spelling::absolute(cache_dir)), project_root) &&
           live_owner(cache_dir_owner(cache_dir), project_root);
}

void set_cache_dir_owner(llvm::StringRef cache_dir, CanonicalRef root) {
    // One inside the root is the root's, whatever it records — a copied
    // checkout carries the original's record along.
    if(cache_dir_owner(cache_dir) == llvm::StringRef(root) || owned_elsewhere(cache_dir, root)) {
        return;
    }
    if(auto error = fs::write_file(path::join(cache_dir, cache_owner_file), root.str() + "\n")) {
        LOG_WARN("Cannot record the owner of cache directory {}: {}", cache_dir, error.message());
    }
}

std::expected<std::string, std::string> Settings::json_schema() {
    auto schema = json::schema_string<Settings, DenyUnknownKeys>(/*pretty=*/true);
    if(!schema) {
        return std::unexpected(schema.error().message);
    }
    return std::move(*schema);
}

}  // namespace clice
