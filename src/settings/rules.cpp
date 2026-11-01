module clice;

import :command.edit;
import :index.database;
import :settings.rules;
import :settings.settings;
import :vfs.path;

namespace clice {

namespace {

/// The real path of a declared database file, also of one whose directory
/// does not exist yet: a path without the .json extension names the
/// directory holding it.
std::string real_path(const Spelling& database) {
    if(path::extension(database) == ".json") {
        return CanonicalPath(database).str();
    }
    return CanonicalPath(Spelling("compile_commands.json", database)).str();
}

/// What the rules active under `configuration` do besides naming
/// databases, one string each in declaration order: configurations alike in
/// it and in the databases they read build alike.
std::vector<std::string> edits_of(const Settings& settings, llvm::StringRef configuration) {
    std::vector<std::string> edits;
    for(auto rule: settings.rules) {
        if(!rule.configuration.empty() && rule.configuration != configuration) {
            continue;
        }
        rule.configuration.clear();
        rule.compile_commands.clear();
        rule.resolve_symlinks = false;
        edits.push_back(json::to_string(rule).value_or(""));
    }
    return edits;
}

}  // namespace

Rules::Rules(const Settings& settings, CanonicalRef root, llvm::StringRef configuration) :
    settings(&settings), root(root), tag(configuration) {
    assert(configuration.empty() ? settings.configurations().empty()
                                 : settings.declares(configuration));
    for(auto& rule: settings.compiled_rules) {
        if(rule.active_under(configuration)) {
            active.push_back(&rule);
            declares = declares || rule.declares_sources();
        }
    }
}

llvm::SmallVector<const CompiledRule*> Rules::matching(CanonicalRef path) const {
    llvm::SmallVector<const CompiledRule*> result;
    for(auto* rule: active) {
        if(rule->matches(path)) {
            result.push_back(rule);
        }
    }
    return result;
}

llvm::SmallVector<Spelling> Rules::declared_databases() const {
    llvm::SmallVector<Spelling> result;
    for(auto* rule: active) {
        for(auto& database: rule->compile_commands) {
            if(!ranges::contains(result, database)) {
                result.push_back(database);
            }
        }
    }
    return result;
}

llvm::SmallVector<const Spelling*> Rules::database_order(CanonicalRef path) const {
    auto matched = matching(path);
    llvm::SmallVector<const Spelling*> order;
    auto add = [&](const CompiledRule& rule) {
        for(auto& database: rule.compile_commands) {
            order.push_back(&database);
        }
    };
    for(auto* rule: matched) {
        add(*rule);
    }
    for(auto* rule: active) {
        if(!ranges::contains(matched, rule)) {
            add(*rule);
        }
    }
    return order;
}

llvm::SmallVector<const Spelling*> Rules::hiding_databases() const {
    llvm::SmallVector<const Spelling*> hiding;
    for(auto& rule: settings->compiled_rules) {
        if(declares || rule.active_under(tag)) {
            for(auto& database: rule.compile_commands) {
                hiding.push_back(&database);
            }
        }
    }
    return hiding;
}

Edits Rules::edits(llvm::ArrayRef<CanonicalRef> paths) const {
    llvm::SmallVector<const CompiledRule*> matched;
    for(auto path: paths) {
        for(auto* rule: matching(path)) {
            if(!ranges::contains(matched, rule)) {
                matched.push_back(rule);
            }
        }
    }
    Edits result;
    for(auto* rule: active) {
        if(!ranges::contains(matched, rule)) {
            continue;
        }
        result.query_toolchain = result.query_toolchain && rule->query_toolchain;
        if(!rule->remove.empty()) {
            result.edits.push_back({.kind = CommandEdit::Kind::Remove,
                                    .flags = rule->remove,
                                    .directory = rule->directory});
        }
        if(!rule->append.empty()) {
            result.edits.push_back({.kind = CommandEdit::Kind::Append,
                                    .flags = rule->append,
                                    .directory = rule->directory});
        }
    }
    return result;
}

std::uint64_t Rules::identity(const Edits& edits) const {
    return edits.empty() ? 0 : llvm::xxh3_64bits(edits.options().key(root));
}

const CompiledRule* Rules::default_rule(CanonicalRef path) const {
    for(auto* rule: active) {
        if(rule->has_default_command() && rule->matches(path)) {
            return rule;
        }
    }
    return nullptr;
}

bool Rules::indexable(CanonicalRef path) const {
    return ranges::all_of(matching(path), [](const CompiledRule* rule) { return rule->index; });
}

bool Rules::inside(CanonicalRef path, bool CompiledRule::* field) const {
    if(!root.empty() && !path::is_within(path, root)) {
        return false;
    }
    return ranges::all_of(matching(path), [&](const CompiledRule* rule) { return rule->*field; });
}

bool Rules::lintable(CanonicalRef path) const {
    return inside(path, &CompiledRule::lint);
}

bool Rules::formattable(CanonicalRef path) const {
    return inside(path, &CompiledRule::format);
}

llvm::SmallVector<const CompiledRule*> Rules::covering() const {
    llvm::SmallVector<const CompiledRule*> result;
    for(auto* rule: active) {
        if(rule->has_default_command() && !rule->unmatchable) {
            result.push_back(rule);
        }
    }
    return result;
}

SourceWalk Rules::source_walk() const {
    SourceWalk walk;
    auto rules = covering();
    if(rules.empty()) {
        return walk;
    }

    // Where the covered files can be: each pattern's literal directory, the
    // whole project for a rule without patterns. A root inside another
    // is walked as part of it.
    auto add_root = [&](CanonicalRef dir) {
        if(!dir.empty() && !ranges::contains(walk.roots, CanonicalPath(dir))) {
            walk.roots.emplace_back(dir);
        }
    };
    for(auto* rule: rules) {
        if(rule->patterns.empty()) {
            add_root(root);
        }
        for(auto& pattern: rule->patterns) {
            add_root(pattern.root);
            walk.patterned.emplace_back(pattern.root);
        }
    }
    walk.roots.erase(
        ranges::remove_if(walk.roots,
                          [&](const CanonicalPath& root) {
                              return ranges::any_of(walk.roots, [&](const CanonicalPath& other) {
                                  return other != root && path::is_within(root, other);
                              });
                          })
            .begin(),
        walk.roots.end());
    walk.cache_dir = settings->project.cache_identity;
    return walk;
}

std::string Rules::resolved_databases() const {
    std::string resolved;
    for(auto* rule: active) {
        if(!rule->resolve_symlinks) {
            continue;
        }
        for(auto& database: rule->compile_commands) {
            if(!resolved.empty()) {
                resolved += '\n';
            }
            resolved += real_path(database);
        }
    }
    return resolved;
}

index::DatabaseKey Rules::database_key(llvm::StringRef resolved) const {
    if(resolved.empty()) {
        return {.configuration = tag};
    }
    auto real_databases = [](const Rules& rules) {
        std::vector<std::string> paths;
        for(auto& database: rules.declared_databases()) {
            paths.push_back(real_path(database));
        }
        return paths;
    };
    auto databases = real_databases(*this);
    auto edits = edits_of(*settings, tag);
    for(auto other: settings->configurations()) {
        if(other == tag) {
            continue;
        }
        Rules rules(*settings, root, other);
        if(ranges::none_of(rules.active, &CompiledRule::resolve_symlinks) &&
           real_databases(rules) == databases && edits_of(*settings, other) == edits) {
            return {.configuration = other.str()};
        }
    }
    return {.configuration = tag, .resolved = resolved.str()};
}

}  // namespace clice
