module clice;

import :command.edit;
import :command.file_kind;
import :command.normalize;
import :project.build;
import :project.configuration;
import :support.logging;
import :support.shell;
import :vfs.file_system;
import :vfs.path;

namespace clice {

void Build::reset_active(llvm::StringRef configuration) {
    assert(configuration.empty() ? config.configurations().empty()
                                 : declares_configuration(config, configuration));
    covered_sources.reset();
    records.clear();
    provisional.clear();
    records_generation += 1;
    active = configuration.str();
}

llvm::SmallVector<const CompiledRule*> Build::matching(CanonicalRef path) const {
    return config.matching_rules(path, active);
}

bool Build::declares_sources() const {
    return ranges::any_of(config.compiled_rules, [&](const CompiledRule& rule) {
        return rule.active_under(active) && rule.declares_sources();
    });
}

llvm::SmallVector<Spelling> Build::declared_sources() const {
    return config.declared_databases(active);
}

llvm::SmallVector<SourceID, 4> Build::declared_ids() const {
    llvm::SmallVector<SourceID, 4> declared;
    bool declares = declares_sources();
    for(auto& rule: config.compiled_rules) {
        if(rule.active_under(active) || declares) {
            for(auto& database: rule.compile_commands) {
                if(auto id = declared_source(database)) {
                    declared.push_back(*id);
                }
            }
        }
    }
    return declared;
}

std::optional<SourceID> Build::declared_source(const Spelling& database) const {
    if(declared_source_count != cdb.source_count()) {
        declared_source_ids.clear();
        declared_source_count = cdb.source_count();
    }
    auto [it, inserted] = declared_source_ids.try_emplace(database);
    if(inserted) {
        it->second = cdb.find_source(database);
    }
    return it->second;
}

bool Build::discovered(SourceID id) const {
    return !ranges::contains(declared_ids(), id);
}

llvm::SmallVector<SourceID, 4> Build::source_order(CanonicalRef path) const {
    auto matched = matching(path);
    llvm::SmallVector<SourceID, 4> order;
    auto declared = declared_ids();
    auto add_sources = [&](const CompiledRule& rule) {
        for(auto& database: rule.compile_commands) {
            if(auto id = declared_source(database); id && !ranges::contains(order, *id)) {
                order.push_back(*id);
            }
        }
    };
    for(auto& rule: config.compiled_rules) {
        if(rule.active_under(active) && ranges::contains(matched, &rule)) {
            add_sources(rule);
        }
    }
    for(auto& rule: config.compiled_rules) {
        if(rule.active_under(active) && !ranges::contains(matched, &rule)) {
            add_sources(rule);
        }
    }
    llvm::SmallVector<SourceID, 4> discovered;
    for(std::size_t i = 0; i < cdb.source_count(); i += 1) {
        auto id = SourceID(i);
        if(!ranges::contains(order, id) && !ranges::contains(declared, id)) {
            discovered.push_back(id);
        }
    }
    // A vanished database keeps serving its entries, but one regenerated
    // elsewhere takes over the files both list.
    ranges::sort(discovered, {}, [&](SourceID id) {
        return std::pair(!cdb.present(id), discovery_rank(cdb.source_path(id)));
    });
    order.append(discovered);
    return order;
}

std::pair<std::uint32_t, llvm::StringRef> discovery_rank(llvm::StringRef database) {
    auto depth = ranges::count_if(database, [](char c) { return path::is_separator(c); });
    return {static_cast<std::uint32_t>(depth), database};
}

llvm::SmallVector<CompilationEntry, 2> Build::entries(Fid file) const {
    auto all = cdb.candidate_entries(file);
    if(all.empty()) {
        return {};
    }
    llvm::SmallVector<CompilationEntry, 2> result;
    for(auto id: source_order(files.resolve(file))) {
        for(auto& entry: all) {
            if(entry.source == id) {
                result.push_back(entry);
            }
        }
    }
    return result;
}

llvm::SmallVector<Candidate, 2> Build::commands(Fid file) {
    llvm::SmallVector<Candidate, 2> result;
    for(auto& entry: entries(file)) {
        result.push_back({.command = entry.command, .source = CommandSource::CDBExact});
    }
    if(result.empty()) {
        if(auto id = default_command(files.resolve(file))) {
            result.push_back({.command = *id, .source = CommandSource::Default});
        } else if(auto it = provisional.find(file); it != provisional.end()) {
            result.push_back({.command = it->second, .source = CommandSource::Inferred});
        }
    }
    return result;
}

Edits Build::edits(llvm::ArrayRef<CanonicalRef> paths) const {
    llvm::SmallVector<const CompiledRule*> matched;
    for(auto path: paths) {
        for(auto* rule: matching(path)) {
            if(!ranges::contains(matched, rule)) {
                matched.push_back(rule);
            }
        }
    }
    return edits_of(matched);
}

Edits Build::edits_of(llvm::ArrayRef<const CompiledRule*> matched) const {
    Edits result;
    for(auto& rule: config.compiled_rules) {
        if(!ranges::contains(matched, &rule)) {
            continue;
        }
        result.query_toolchain = result.query_toolchain && rule.query_toolchain;
        if(!rule.remove.empty()) {
            result.edits.push_back({.kind = CommandEdit::Kind::Remove,
                                    .flags = rule.remove,
                                    .directory = rule.directory});
        }
        if(!rule.append.empty()) {
            result.edits.push_back({.kind = CommandEdit::Kind::Append,
                                    .flags = rule.append,
                                    .directory = rule.directory});
        }
    }
    return result;
}

const CompiledRule* Build::default_rule(CanonicalRef path) const {
    for(auto* rule: matching(path)) {
        if(rule->has_default_command()) {
            return rule;
        }
    }
    return nullptr;
}

std::optional<CommandID> Build::intern(const Spelling& directory,
                                       llvm::ArrayRef<const char*> arguments) {
    std::string key = directory.str();
    for(const char* argument: arguments) {
        key += '\0';
        key += argument;
    }
    auto [it, inserted] = interned.try_emplace(key, invalid_command);
    if(inserted) {
        Normalizer normalizer{.commands = command_table, .files = files};
        if(auto normalized = normalizer.normalize(directory, Fid{}, arguments)) {
            it->second = *normalized;
        } else {
            LOG_WARN("Not a compile command: {}", print_argv(arguments));
        }
    }
    if(it->second == invalid_command) {
        return std::nullopt;
    }
    return it->second;
}

CommandID Build::apply(CommandID command, const CommandEdits& edits) {
    if(edits.empty()) {
        return command;
    }
    auto key = edit_keys.try_emplace(edits.key(), edit_keys.size()).first->second;
    auto [it, inserted] = edited.try_emplace({static_cast<std::uint32_t>(command), key});
    if(inserted) {
        it->second = apply_edits(command_table, files, command, edits);
    }
    return it->second;
}

std::optional<CommandID> Build::command_of(const CompiledRule& rule) {
    llvm::SmallVector<const char*, 16> argv;
    for(auto& arg: rule.default_command) {
        argv.push_back(arg.c_str());
    }
    return intern(rule.directory, argv);
}

std::optional<CommandID> Build::default_command(CanonicalRef path) {
    auto* rule = default_rule(path);
    return rule ? command_of(*rule) : std::nullopt;
}

CommandID Build::builtin(CanonicalRef path) {
    // Every C++ spelling (.cc, .cxx, .C, .hh) gets clang++, and so do the
    // ambiguous .h and a name clang has no language for (`Widget`, a `.tcc`
    // body), which only an include ever reads; C and Objective-C get clang.
    namespace types = clang::driver::types;
    auto type = suffix_type(path);
    llvm::SmallVector<const char*, 8> arguments;
    if(path::extension(path) == ".cuh" || (type != types::TY_INVALID && types::isCuda(type))) {
        // Device-only pins the same device-side view NVCC-backed commands
        // default to, instead of whichever job the toolchain query happens
        // to pick from a two-sided compilation; a rule appending
        // --cuda-host-only still wins as the later flag.
        arguments = {"clang++", "-std=c++20", "-x", "cuda", "--cuda-device-only"};
    } else if(type != types::TY_INVALID && types::isCXX(type)) {
        arguments = {"clang++", "-std=c++20"};
    } else if(type == types::TY_CHeader || type == types::TY_INVALID) {
        // C++ by default, like clangd; -x forces TU semantics instead of a
        // precompiled-header job.
        arguments = {"clang++", "-std=c++20", "-x", "c++"};
    } else {
        arguments = {"clang"};
    }
    // Run from the file's own directory, like a compile nobody wrote down.
    return *intern(Spelling(path.parent()), arguments);
}

CommandRef Build::resolve(Fid file,
                          CommandID base,
                          CommandSource source,
                          llvm::ArrayRef<CanonicalRef> paths,
                          llvm::StringRef language_path,
                          llvm::ArrayRef<std::string> extra_prepend,
                          llvm::ArrayRef<std::string> extra_append) {
    auto edit = edits(paths);
    auto applied = apply(base, edit.options(extra_prepend, extra_append));
    return {file, applied, command_table.input_kind(applied, language_path), source};
}

std::string Build::edit_hash(llvm::ArrayRef<CanonicalRef> paths) const {
    auto edit = edits(paths);
    if(edit.empty()) {
        return {};
    }
    llvm::StringRef root = config.workspace_root;
    std::string joined;
    for(auto& item: edit.edits) {
        joined += item.kind == CommandEdit::Kind::Remove ? 'r' : 'a';
        llvm::SmallString<256> storage;
        joined += path::portable(item.directory, root, storage);
        joined += '\0';
        for(llvm::StringRef flag: item.flags) {
            // The rule's `${workspace}` put back, so a moved checkout
            // keeps the hash.
            while(!root.empty()) {
                auto at = flag.find(root);
                if(at == llvm::StringRef::npos) {
                    break;
                }
                joined += flag.take_front(at);
                joined += path::workspace_anchor;
                flag = flag.drop_front(at + root.size());
            }
            joined += flag;
            joined += '\0';
        }
        joined += '\1';
    }
    if(!edit.query_toolchain) {
        joined += "unqueried";
    }
    return std::format("{:016x}", llvm::xxh3_64bits(joined));
}

bool Build::indexed(CanonicalRef path) const {
    return ranges::all_of(matching(path), [](const CompiledRule* rule) { return rule->index; });
}

bool Build::inside(CanonicalRef path, bool CompiledRule::* field) const {
    if(!config.workspace_root.empty() && !path::is_within(path, config.workspace_root)) {
        return false;
    }
    return ranges::all_of(matching(path), [&](const CompiledRule* rule) { return rule->*field; });
}

bool Build::lintable(CanonicalRef path) const {
    return inside(path, &CompiledRule::lint);
}

bool Build::formattable(CanonicalRef path) const {
    return inside(path, &CompiledRule::format);
}

llvm::SmallVector<CommandRef> Build::units(llvm::ArrayRef<Fid> members) {
    llvm::SmallVector<CommandRef> result;
    for(auto member: members) {
        auto path = files.resolve(member);
        // Two databases listing the file with the same command make one
        // unit: the scan would only read it twice.
        auto first = result.size();
        for(auto& candidate: commands(member)) {
            auto unit = resolve(member, candidate.command, candidate.source, path, path);
            bool seen = ranges::any_of(llvm::ArrayRef(result).drop_front(first),
                                       [&](const CommandRef& other) {
                                           return other.command == unit.command &&
                                                  other.input.value == unit.input.value;
                                       });
            if(!seen) {
                result.push_back(unit);
            }
        }
    }
    return result;
}

std::vector<Fid> Build::members() {
    std::vector<Fid> result;
    llvm::DenseSet<Fid> seen;
    for(auto& entry: cdb.entries()) {
        if(seen.insert(entry.file).second && !entries(entry.file).empty()) {
            result.push_back(entry.file);
        }
    }
    if(!covered_sources) {
        covered_sources.emplace();
        collect_covered_sources(walk_sources(source_walk()), *covered_sources);
    }
    for(auto file: *covered_sources) {
        if(seen.insert(file).second) {
            result.push_back(file);
        }
    }
    auto borrowing = ranges::to<llvm::SmallVector<Fid>>(
        views::transform(provisional, [](auto& entry) { return entry.first; }));
    ranges::sort(borrowing, {}, [&](Fid file) { return files.resolve(file); });
    result.append_range(borrowing);
    return result;
}

bool Build::record(Fid file) {
    if(!records.insert(file).second) {
        return false;
    }
    records_generation += 1;
    return true;
}

void Build::forget(Fid file) {
    if(records.erase(file)) {
        records_generation += 1;
    }
    provisional.erase(file);
}

std::optional<CommandID> Build::borrow(Fid file, std::optional<CommandID> command) {
    assert(records.contains(file) && "only a recorded file borrows");
    std::optional<CommandID> before;
    if(auto it = provisional.find(file); it != provisional.end()) {
        before = it->second;
    }
    if(command) {
        provisional[file] = *command;
    } else {
        provisional.erase(file);
    }
    return before;
}

CommandID Build::lend(CommandID command, CanonicalRef lender, CanonicalRef file) {
    auto own = matching(file);
    auto matched = matching(lender);
    matched.erase(
        ranges::remove_if(matched,
                          [&](const CompiledRule* rule) { return ranges::contains(own, rule); })
            .begin(),
        matched.end());
    return apply(command, edits_of(matched).options());
}

llvm::SmallVector<Fid> Build::refresh_default_sources(llvm::ArrayRef<CanonicalPath> walked) {
    std::vector<Fid> current;
    collect_covered_sources(walked, current);
    llvm::SmallVector<Fid> appeared;
    if(covered_sources) {
        llvm::DenseSet<Fid> known(covered_sources->begin(), covered_sources->end());
        for(auto file: current) {
            if(!known.contains(file)) {
                appeared.push_back(file);
            }
        }
    }
    covered_sources = std::move(current);
    return appeared;
}

bool Build::default_source(CanonicalRef path) {
    if(is_source_path(path)) {
        return true;
    }
    // A header is no translation unit of its own; a file clang has no
    // language for is one when its rule's command forces a source language
    // and a pattern singles it out, naming its suffix (`*.ino`) or the file
    // itself: one matching any name there (`src/**`) reaches CMakeLists.txt
    // and README.md too.
    if(suffix_type(path) != clang::driver::types::TY_INVALID || path::extension(path) == ".cuh") {
        return false;
    }
    auto* rule = default_rule(path);
    if(!rule) {
        return false;
    }
    auto another_name = llvm::StringRef(path).str() + "~";
    bool singled_out = ranges::any_of(rule->patterns, [&](const CompiledRule::Pattern& pattern) {
        return pattern.glob.match(llvm::StringRef(path)) && !pattern.glob.match(another_name);
    });
    if(!singled_out) {
        return false;
    }
    auto command = command_of(*rule);
    if(!command) {
        return false;
    }
    auto forced = command_table.forced_language(*command);
    return !forced.empty() && !forced.ends_with("-header");
}

bool Build::unit(Fid file) {
    return declared(file) || provisional.contains(file);
}

bool Build::declared(Fid file) {
    if(!entries(file).empty()) {
        return true;
    }
    auto path = files.resolve(file);
    return default_command(path).has_value() && default_source(path);
}

std::optional<CommandRef> Build::pch_unit(CommandID command,
                                          llvm::ArrayRef<CanonicalRef> paths,
                                          llvm::ArrayRef<std::string> extra_prepend,
                                          llvm::ArrayRef<std::string> extra_append) {
    if(command_table.pch_output(command)) {
        return std::nullopt;
    }
    for(auto& input: command_table.pch_inputs(command)) {
        auto pch = files.find(input);
        if(!pch) {
            continue;
        }
        for(auto& writer: cdb.pch_writers(*pch)) {
            bool active = ranges::any_of(entries(writer.file), [&](const CompilationEntry& entry) {
                return entry.source == writer.source && entry.ordinal == writer.ordinal;
            });
            if(active) {
                return resolve(writer.file,
                               writer.command,
                               CommandSource::CDBExact,
                               paths,
                               files.resolve(writer.file),
                               extra_prepend,
                               extra_append);
            }
        }
    }
    return std::nullopt;
}

llvm::SmallVector<const CompiledRule*> Build::covering_rules() const {
    llvm::SmallVector<const CompiledRule*> result;
    for(auto& rule: config.compiled_rules) {
        if(rule.active_under(active) && rule.has_default_command() && !rule.unmatchable) {
            result.push_back(&rule);
        }
    }
    return result;
}

Build::SourceWalk Build::source_walk() const {
    SourceWalk walk;
    auto rules = covering_rules();
    if(rules.empty()) {
        return walk;
    }

    // Where the covered files can be: each pattern's literal directory, the
    // whole workspace for a rule without patterns. A root inside another
    // is walked as part of it.
    auto add_root = [&](CanonicalRef root) {
        if(!root.empty() && !ranges::contains(walk.roots, CanonicalPath(root))) {
            walk.roots.emplace_back(root);
        }
    };
    for(auto* rule: rules) {
        if(rule->patterns.empty()) {
            add_root(config.workspace_root);
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
    if(!config.project.cache_dir.empty()) {
        walk.cache_dir = CanonicalPath(Spelling::absolute(config.project.cache_dir));
    }
    return walk;
}

void Build::collect_covered_sources(llvm::ArrayRef<CanonicalPath> walked, std::vector<Fid>& out) {
    auto rules = covering_rules();
    llvm::DenseSet<Fid> seen(out.begin(), out.end());
    for(auto& path: walked) {
        auto matched = matching(path);
        if(!ranges::any_of(rules, [&](const CompiledRule* rule) {
               return ranges::contains(matched, rule);
           })) {
            continue;
        }
        if(!default_source(path)) {
            continue;
        }
        auto file = files.intern(path);
        if(seen.insert(file).second) {
            out.push_back(file);
        }
    }
}

std::vector<CanonicalPath> walk_sources(const Build::SourceWalk& walk) {
    std::vector<CanonicalPath> walked;
    for(CanonicalRef root: walk.roots) {
        auto entries = fs::walk(root);
        for(auto& entry: entries) {
            if(!entry) {
                vfs::warn_unreadable(entry.error());
                continue;
            }
            if(entry->type == fs::FileType::Directory) {
                auto name = entry->name();
                if(name == ".git" || (name == path::filename(walk.cache_dir) &&
                                      root.entry(entry->path) == walk.cache_dir)) {
                    entries.skip();
                }
                continue;
            }
            // Only an entry that is itself a symlink names a file elsewhere.
            auto path = entry->type == fs::FileType::Regular
                            ? root.entry(entry->path)
                            : CanonicalPath(Spelling::absolute(entry->path));
            if(suffix_type(path) != clang::driver::types::TY_INVALID ||
               ranges::any_of(walk.patterned, [&](const CanonicalPath& patterned) {
                   return path::is_within(path, patterned);
               })) {
                walked.push_back(std::move(path));
            }
        }
    }
    return walked;
}

/// Whether a workspace directory holds no sources a refactoring edits:
/// hidden, a package cache, the cache directory or a build tree.
static bool outside_sources(CanonicalRef dir, CanonicalRef cache_dir) {
    auto name = path::filename(dir);
    return name.starts_with(".") || name == "node_modules" || dir == cache_dir ||
           fs::exists(path::join(dir, "CMakeCache.txt")) ||
           fs::exists(path::join(dir, "build.ninja"));
}

bool workspace_file(CanonicalRef root, CanonicalRef cache_dir, CanonicalRef file) {
    if(!path::is_within(file, root) || file == root) {
        return false;
    }
    bool inside = true;
    path::walk_ancestors(CanonicalRef(file).parent(), [&](CanonicalRef dir) {
        if(dir == root) {
            return false;
        }
        inside = !outside_sources(dir, cache_dir);
        return inside;
    });
    return inside;
}

EditableWorkspace::EditableWorkspace(const Config& config) : root(config.workspace_root) {
    if(!config.project.cache_dir.empty()) {
        cache_dir = CanonicalPath(Spelling::absolute(config.project.cache_dir));
    }
}

std::vector<std::string> EditableWorkspace::sources() const {
    CanonicalRef workspace = root;
    std::vector<std::string> found;
    auto entries = fs::walk(workspace);
    for(auto& entry: entries) {
        if(!entry) {
            vfs::warn_unreadable(entry.error());
            continue;
        }
        if(entry->type == fs::FileType::Directory) {
            if(outside_sources(workspace.entry(entry->path), cache_dir)) {
                entries.skip();
            }
            continue;
        }
        if(entry->type != fs::FileType::Regular) {
            continue;
        }
        auto path = workspace.entry(entry->path);
        if(clang::driver::types::isAcceptedByClang(suffix_type(path)) ||
           is_context_header_path(path) || path.str().ends_with(".cuh")) {
            found.push_back(std::move(path).str());
        }
    }
    return found;
}

bool EditableWorkspace::editable(llvm::StringRef path) const {
    return workspace_file(root, cache_dir, CanonicalPath(Spelling::absolute(path)));
}

}  // namespace clice
