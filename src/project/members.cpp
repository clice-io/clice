module clice;

import :command.database;
import :command.edit;
import :command.file_kind;
import :project.discovery;
import :project.project;
import :settings.rules;
import :support.logging;
import :syntax.dependency_graph;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

/// The registered sources the rules hide from discovery
/// (Rules::hiding_databases).
llvm::SmallVector<SourceID, 4> hidden_sources(const Project& project) {
    llvm::SmallVector<SourceID, 4> hidden;
    for(auto* database: project.rules.hiding_databases()) {
        if(auto id = project.cdb.find_source(*database)) {
            hidden.push_back(*id);
        }
    }
    return hidden;
}

/// The recorded provisional members, by path.
std::vector<Fid> provisional_files(const Project& project) {
    auto files = ranges::to<std::vector<Fid>>(project.provisional.files());
    ranges::sort(files, {}, [&](Fid file) { return project.file_table.resolve(file); });
    return files;
}

/// The interned default command of `rule`; nullopt when it is not a compile
/// command.
std::optional<CommandID> command_of(Project& project, const CompiledRule& rule) {
    llvm::SmallVector<const char*, 16> argv;
    for(auto& arg: rule.default_command) {
        argv.push_back(arg.c_str());
    }
    return project.written_commands.intern(rule.directory, argv);
}

/// Whether the default command of `rule`, the first rule matching `path`
/// declaring one, compiles `path` as a unit: a C-family source by suffix
/// (never a header), or a file the rule singles out by pattern whose default
/// command forces a language (`-x c++` for an extensionless tool). A rule
/// without patterns applies to every file, so its forced language covers
/// only what clang recognizes — or the configuration file itself would
/// become a unit.
bool default_source(Project& project, CanonicalRef path, const CompiledRule& rule) {
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
    auto another_name = llvm::StringRef(path).str() + "~";
    bool singled_out = ranges::any_of(rule.patterns, [&](const CompiledRule::Pattern& pattern) {
        return pattern.glob.match(llvm::StringRef(path)) && !pattern.glob.match(another_name);
    });
    if(!singled_out) {
        return false;
    }
    auto command = command_of(project, rule);
    if(!command) {
        return false;
    }
    auto forced = project.commands.forced_language(*command);
    return !forced.empty() && !forced.ends_with("-header");
}

/// The files of a walk a default-command rule covers: C-family sources
/// (never headers) matching the rules' patterns.
void collect_covered_sources(Project& project,
                             llvm::ArrayRef<CanonicalPath> walked,
                             std::vector<Fid>& out) {
    auto rules = project.rules.covering();
    llvm::DenseSet<Fid> seen(out.begin(), out.end());
    for(auto& path: walked) {
        auto matched = project.rules.matching(path);
        if(!ranges::any_of(rules, [&](const CompiledRule* rule) {
               return ranges::contains(matched, rule);
           })) {
            continue;
        }
        auto* rule = project.rules.default_rule(path);
        if(!rule || !default_source(project, path, *rule)) {
            continue;
        }
        auto file = project.file_table.intern(path);
        if(seen.insert(file).second) {
            out.push_back(file);
        }
    }
}

}  // namespace

bool Project::discovered(SourceID id) const {
    return !ranges::contains(hidden_sources(*this), id);
}

llvm::SmallVector<SourceID, 4> Project::source_order(CanonicalRef path) const {
    llvm::SmallVector<SourceID, 4> order;
    for(auto* database: rules.database_order(path)) {
        if(auto id = cdb.find_source(*database); id && !ranges::contains(order, *id)) {
            order.push_back(*id);
        }
    }
    auto hidden = hidden_sources(*this);
    llvm::SmallVector<SourceID, 4> discovered;
    for(std::size_t i = 0; i < cdb.source_count(); i += 1) {
        auto id = SourceID(i);
        if(!ranges::contains(order, id) && !ranges::contains(hidden, id)) {
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

llvm::SmallVector<CompilationEntry, 2> Project::entries(Fid file) const {
    auto all = cdb.candidate_entries(file);
    if(all.empty()) {
        return {};
    }
    llvm::SmallVector<CompilationEntry, 2> result;
    for(auto id: source_order(file_table.resolve(file))) {
        for(auto& entry: all) {
            if(entry.source == id) {
                result.push_back(entry);
            }
        }
    }
    return result;
}

llvm::SmallVector<Candidate, 2> Project::candidates(Fid file) {
    llvm::SmallVector<Candidate, 2> result;
    for(auto& entry: entries(file)) {
        result.push_back({.base = entry.command, .source = CommandSource::Database});
    }
    if(result.empty()) {
        if(auto id = default_command(file_table.resolve(file))) {
            result.push_back({.base = *id, .source = CommandSource::Default});
        } else if(auto* command = provisional.command(file)) {
            result.push_back(*command);
        }
    }
    return result;
}

std::optional<CommandID> Project::default_command(CanonicalRef path) {
    auto* rule = rules.default_rule(path);
    return rule ? command_of(*this, *rule) : std::nullopt;
}

CommandID Project::fallback_command(CanonicalRef path) {
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
    return *written_commands.intern(Spelling(path.parent()), arguments);
}

Edits Project::rule_edits(Fid file, const Candidate& candidate) const {
    llvm::SmallVector<CanonicalRef, 3> paths;
    for(auto from: candidate.rules_from) {
        paths.push_back(file_table.resolve(from));
    }
    paths.push_back(file_table.resolve(file));
    return rules.edits(paths);
}

CommandRef Project::apply_rules(Fid file,
                                const Candidate& candidate,
                                llvm::ArrayRef<std::string> extra_prepend,
                                llvm::ArrayRef<std::string> extra_append) {
    auto applied =
        edited_commands.apply(candidate.base,
                              rule_edits(file, candidate).options(extra_prepend, extra_append));
    // A header borrowing a command compiles as that command's unit does — a
    // `.h` as C++, not as a header job — and so does a file in its host; a
    // source in its own language, which borrowing only ever matches or
    // crosses between C++ and its modules.
    auto path = file_table.resolve(file);
    bool foreign = candidate.from().valid() &&
                   (candidate.source == CommandSource::Host || is_header_path(path));
    auto language_path = foreign ? file_table.resolve(candidate.from()) : path;
    return {file, applied, commands.input_kind(applied, language_path), candidate.source};
}

std::vector<Fid> Project::members() {
    std::vector<Fid> result;
    llvm::DenseSet<Fid> seen;
    for(auto& entry: cdb.entries()) {
        if(seen.insert(entry.file).second && !entries(entry.file).empty()) {
            result.push_back(entry.file);
        }
    }
    if(!covered_sources) {
        covered_sources.emplace();
        collect_covered_sources(*this, walk_sources(rules.source_walk()), *covered_sources);
    }
    for(auto file: *covered_sources) {
        if(seen.insert(file).second) {
            result.push_back(file);
        }
    }
    for(auto file: provisional_files(*this)) {
        if(provisional.command(file)) {
            result.push_back(file);
        }
    }
    return result;
}

llvm::SmallVector<CommandRef> Project::scan_commands(llvm::ArrayRef<Fid> members) {
    llvm::SmallVector<CommandRef> result;
    for(auto member: members) {
        // Two databases listing the file with the same command make one
        // unit: the scan would only read it twice.
        auto first = result.size();
        for(auto& candidate: candidates(member)) {
            auto unit = apply_rules(member, candidate);
            bool seen = ranges::any_of(llvm::ArrayRef(result).drop_front(first),
                                       [&](const CommandRef& other) {
                                           return other.command == unit.command &&
                                                  other.language.value == unit.language.value;
                                       });
            if(!seen) {
                result.push_back(unit);
            }
        }
    }
    return result;
}

bool Project::record_provisional(Fid file) {
    // A header compiles in a host or under a borrowed command, never as a
    // member; a build tree or the cache holds no sources of the project's
    // own.
    auto path = file_table.resolve(file);
    if(root.empty() || !is_source_path(path) || declared(file) ||
       !project_file(root, settings.project.cache_identity, path) ||
       !nested_project(root, path).empty() || !provisional.record(file)) {
        return false;
    }
    LOG_INFO("Recorded {} as a provisional member", path);
    return true;
}

llvm::SmallVector<Fid> Project::refresh_default_sources(llvm::ArrayRef<CanonicalPath> walked) {
    std::vector<Fid> current;
    collect_covered_sources(*this, walked, current);
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

bool Project::is_unit(Fid file) {
    return declared(file) || provisional.command(file);
}

bool Project::declared(Fid file) {
    if(!entries(file).empty()) {
        return true;
    }
    auto path = file_table.resolve(file);
    auto* rule = rules.default_rule(path);
    return rule && command_of(*this, *rule) && default_source(*this, path, *rule);
}

std::optional<CommandRef> Project::pch_writer(Fid file,
                                              const Candidate& candidate,
                                              CommandID command,
                                              llvm::ArrayRef<std::string> extra_prepend,
                                              llvm::ArrayRef<std::string> extra_append) {
    if(commands.pch_output(command)) {
        return std::nullopt;
    }
    for(auto& input: commands.pch_inputs(command)) {
        auto pch = file_table.find(input);
        if(!pch) {
            continue;
        }
        for(auto& writer: cdb.pch_writers(*pch)) {
            bool active = ranges::any_of(entries(writer.file), [&](const CompilationEntry& entry) {
                return entry.source == writer.source && entry.ordinal == writer.ordinal;
            });
            if(active) {
                auto applied = edited_commands.apply(
                    writer.command,
                    rule_edits(file, candidate).options(extra_prepend, extra_append));
                return CommandRef{writer.file,
                                  applied,
                                  commands.input_kind(applied, file_table.resolve(writer.file)),
                                  CommandSource::Database};
            }
        }
    }
    return std::nullopt;
}

void Project::rescan_disk_file(Fid path_id) {
    rescan_dependency_graph(cdb, toolchain, dep_graph, path_id);
    context_epoch += 1;
}

void Project::forget_file(Fid path_id) {
    dep_graph.update_module_decl(path_id, {});
    dep_graph.forget_scan(path_id);
    dep_graph.clear_includes(path_id);
    context_epoch += 1;
}

Project::Rebuilt Project::rebuild_dependency_graph() {
    llvm::StringMap<llvm::SmallVector<Fid, 2>> providers;
    for(auto& entry: dep_graph.modules()) {
        if(!entry.getValue().empty()) {
            providers[entry.getKey()] = entry.getValue();
        }
    }

    // TODO: every CDB change rescans the whole graph synchronously on the
    // event loop, twice when provisional members change; make it
    // incremental or move it off the dispatch path.
    auto scan = [&] {
        dep_graph.reset();
        scan_dependency_graph(cdb, toolchain, dep_graph, scan_commands(members()));
        dep_graph.build_reverse_map();
    };
    scan();
    Rebuilt changes{.provisional = refresh_provisional()};
    if(!changes.provisional.empty()) {
        scan();
    }
    context_epoch += 1;

    for(auto& entry: dep_graph.modules()) {
        if(entry.getValue().empty()) {
            continue;
        }
        auto it = providers.find(entry.getKey());
        if(it == providers.end()) {
            changes.appeared.push_back(entry.getKey().str());
            continue;
        }
        auto now = ranges::to<llvm::SmallVector<Fid>>(entry.getValue());
        ranges::sort(it->second);
        ranges::sort(now);
        if(it->second != now) {
            changes.replaced.append(it->second.begin(), it->second.end());
        }
    }
    return changes;
}

CDBDiff Project::refresh_provisional() {
    CDBDiff diff;
    for(auto file: provisional_files(*this)) {
        if(declared(file)) {
            provisional.forget(file);
            continue;
        }
        // A file a declared unit includes compiles in its includer's
        // context, as the part of a unity build does.
        auto hosted = ranges::any_of(dep_graph.find_host_sources(file),
                                     [&](Fid root) { return root != file && declared(root); });
        // One no unit lends a command compiles under the builtin one, a
        // member all the same: the host of the headers it includes.
        std::optional<Candidate> command;
        if(!hosted) {
            if(auto borrowed = borrowed_command(file, false)) {
                command = borrowed;
            } else {
                command = {.base = fallback_command(file_table.resolve(file)),
                           .source = CommandSource::Fallback};
            }
        }
        auto before = provisional.set_command(file, command);
        if(before == command) {
            continue;
        }
        (!before ? diff.added : !command ? diff.removed : diff.changed).push_back(file);
    }
    return diff;
}

bool Project::units_pending() {
    return ranges::any_of(members(), [&](Fid unit) {
        return rules.indexable(file_table.resolve(unit)) && !index.indexed(unit);
    });
}

std::vector<CanonicalPath> walk_sources(const SourceWalk& walk) {
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
                if(name == ".git" ||
                   (name == path::filename(walk.cache_dir) &&
                    root.entry(entry->path) == walk.cache_dir) ||
                   has_configuration_file(Spelling::absolute(entry->path))) {
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

}  // namespace clice
