module clice;

import :command.database;
import :project.build;
import :project.hosting;
import :project.project;
import :support.logging;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;

namespace clice {

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
        scan_dependency_graph(cdb, toolchain, dep_graph, build.units(build.members()));
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
    auto recorded = ranges::to<llvm::SmallVector<Fid>>(build.recorded());
    ranges::sort(recorded, {}, [&](Fid file) { return file_table.resolve(file); });
    for(auto file: recorded) {
        if(build.declared(file)) {
            build.forget(file);
            continue;
        }
        // A file a declared unit includes compiles in its includer's
        // context, as the part of a unity build does.
        auto hosted = ranges::any_of(dep_graph.find_host_sources(file), [&](Fid root) {
            return root != file && build.declared(root);
        });
        std::optional<CommandID> command;
        if(!hosted) {
            if(auto lender = command_lender(*this, file)) {
                command = build.lend(lender->command,
                                     file_table.resolve(lender->unit),
                                     file_table.resolve(file));
            }
        }
        auto before = build.borrow(file, command);
        if(before == command) {
            continue;
        }
        (!before ? diff.added : !command ? diff.removed : diff.changed).push_back(file);
    }
    return diff;
}

namespace {

constexpr llvm::StringLiteral database_name = "compile_commands.json";

std::optional<Spelling> database_in(const Spelling& dir) {
    Spelling candidate(database_name, dir);
    if(!fs::exists(candidate)) {
        return std::nullopt;
    }
    return candidate;
}

/// The places that hold a database.
llvm::SmallVector<Spelling> existing(llvm::SmallVector<Spelling> places) {
    places.erase(
        ranges::remove_if(places, [](const Spelling& place) { return !fs::exists(place); }).begin(),
        places.end());
    return places;
}

bool configured(const Spelling& dir) {
    return ranges::any_of(settings_file_names,
                          [&](llvm::StringRef name) { return fs::exists(Spelling(name, dir)); });
}

}  // namespace

llvm::SmallVector<Spelling> database_places(CanonicalRef project_root) {
    llvm::SmallVector<Spelling> places;
    if(project_root.empty()) {
        return places;
    }
    places.emplace_back(database_name, Spelling(project_root));

    // Name order, so build/ and out/ side by side load in the same order on
    // every start rather than whichever the directory listing yields first.
    llvm::SmallVector<Spelling> subdirectories;
    if(auto entries = fs::Directory::open(project_root)) {
        for(auto& entry: *entries) {
            if(!entry) {
                break;
            }
            // A symlinked build directory is a build directory too, even
            // before its target exists.
            if(entry->type == fs::FileType::Symlink || entry->type == fs::FileType::Directory) {
                subdirectories.push_back(Spelling::absolute(path::join(project_root, entry->name)));
            }
        }
    }
    ranges::sort(subdirectories);
    for(auto& subdirectory: subdirectories) {
        places.emplace_back(database_name, subdirectory);
    }
    return places;
}

llvm::SmallVector<Spelling> discover_compile_commands(CanonicalRef project_root) {
    return existing(database_places(project_root));
}

llvm::SmallVector<Spelling> deeper_compile_commands(CanonicalRef project_root,
                                                    CanonicalRef cache_dir) {
    // Deep enough for a CMake preset's out/build/<preset> and a nested
    // project's build directory, and no deeper: a project root with no
    // database at all may be a home directory.
    llvm::SmallVector<Spelling> found;
    auto walk = fs::walk(project_root, {.max_depth = 4});
    for(auto& entry: walk) {
        if(!entry) {
            vfs::warn_unreadable(entry.error());
            continue;
        }
        auto spelled = Spelling::absolute(entry->path);
        if(entry->type == fs::FileType::Directory) {
            auto name = entry->name();
            if(name.starts_with(".") || name == "node_modules" ||
               project_root.entry(spelled) == cache_dir) {
                walk.skip();
            }
            continue;
        }
        // The root and its direct subdirectories are where discovery looks.
        if(llvm::StringRef(entry->name()) == database_name) {
            if(entry->depth > 2) {
                found.push_back(std::move(spelled));
            }
        } else if(entry->type == fs::FileType::Symlink && entry->depth >= 2 &&
                  fs::is_directory(spelled)) {
            // Not walked into (links may cycle), but a build directory
            // symlinked elsewhere keeps its database.
            if(auto database = database_in(spelled)) {
                found.push_back(std::move(*database));
            }
        }
    }
    ranges::sort(found);
    return found;
}

bool defines_project(CanonicalRef dir) {
    return configured(Spelling(dir)) || !discover_compile_commands(dir).empty();
}

Spelling find_project_root(const Spelling& start) {
    // The directories holding `start`, not the links on the way to it: a
    // directory reached through a symlink belongs to the project of the one
    // it links to.
    auto defines = [](const Spelling& dir) {
        return configured(dir) || database_in(dir) || database_in(Spelling("build", dir));
    };
    CanonicalPath found;
    path::walk_ancestors(CanonicalPath(start), [&](CanonicalRef dir) {
        Spelling spelled(dir);
        if(!defines(spelled)) {
            return true;
        }
        found = dir;
        // A database alone in a directory is its parent's when the parent
        // is a project, whose discovery loads its direct subdirectories': a
        // build directory beside the sources.
        auto parent = dir.parent();
        if(!configured(spelled) && !database_in(Spelling("build", spelled)) && !parent.empty() &&
           defines(Spelling(parent))) {
            found = std::move(parent);
        }
        return false;
    });
    if(found.empty()) {
        return {};
    }
    for(auto dir = start; !dir.empty(); dir = dir.parent()) {
        if(CanonicalPath(dir) == found) {
            return dir;
        }
    }
    return Spelling(found);
}

std::string deeper_databases_hint(const Project& project) {
    if(project.build.declares_sources()) {
        return {};
    }
    CanonicalRef root = project.root;
    std::vector<std::string> names;
    for(auto& database: deeper_compile_commands(root, project.settings.project.cache_identity())) {
        auto directory = database.parent();
        auto name = llvm::StringRef(directory).drop_front(root.size()).ltrim(R"(/\)").str();
        path::to_slash(name);
        names.push_back(std::format(R"("{}")", name));
    }
    if(names.empty()) {
        return {};
    }
    return std::format(
        " Deeper ones load only when clice.toml names them (the editor also loads one when it "
        "opens a file below it); to use those found, give clice.toml a [[rules]] table with "
        "compile_commands = [{}].",
        llvm::join(names, ", "));
}

bool found_unreadable_database(const Project& project) {
    return ranges::any_of(views::iota(std::size_t(0), project.cdb.source_count()),
                          [&](std::size_t i) {
                              return !project.cdb.loaded(SourceID(i)) &&
                                     fs::exists(project.cdb.source_path(SourceID(i)));
                          });
}

std::string why_no_members(const Project& project, llvm::StringRef root) {
    std::vector<std::string> empty;
    for(std::size_t i = 0; i < project.cdb.source_count(); i += 1) {
        auto path = project.cdb.source_path(SourceID(i));
        if(project.cdb.loaded(SourceID(i))) {
            empty.push_back(path.str());
        } else if(fs::exists(path)) {
            return std::format("{} cannot be read; the error above says why.", path.str());
        } else {
            return std::format("{} does not exist yet; the build writes it.", path.str());
        }
    }
    if(!empty.empty()) {
        return std::format("{} {} no file.",
                           llvm::join(empty, ", "),
                           empty.size() == 1 ? "lists" : "list");
    }
    if(project.build.declares_sources()) {
        return "no file on disk matches a rule with a default_command.";
    }
    return std::format("no compile_commands.json found in {} or its direct subdirectories.{}",
                       root.str(),
                       deeper_databases_hint(project));
}

llvm::SmallVector<Spelling> database_places_above(CanonicalRef start, CanonicalRef project_root) {
    llvm::SmallVector<Spelling> places;
    for(auto dir: path::ancestors(start)) {
        places.emplace_back(database_name, Spelling::absolute(dir));
        if(dir == std::string_view(project_root)) {
            break;
        }
    }
    return places;
}

llvm::SmallVector<Spelling> compile_commands_above(CanonicalRef start, CanonicalRef project_root) {
    return existing(database_places_above(start, project_root));
}

DepsSnapshot capture_deps_snapshot(FileTable& files,
                                   llvm::ArrayRef<DepFile> deps,
                                   std::int64_t build_at) {
    // Files whose mtime falls within the guard of the build start count as
    // "possibly modified during the build".
    auto baseline_before_ns = vfs::stat_baseline_before_ns(build_at);

    DepsSnapshot snap;
    snap.reserve(deps.size());
    vfs::StatusBatch statuses;
    for(const auto& file: deps) {
        auto& dep = snap.emplace_back();
        dep.path_id = files.intern(Spelling::absolute(file.path));
        auto hash = file.hash;

        // A place a failed lookup looked: the build saw nothing there,
        // whatever is there by now. The file table watches it from here on;
        // a file there is a change.
        if(file.absent) {
            dep.missing = true;
            files.disk.current(dep.path_id);
            continue;
        }

        auto status = statuses.status(file.path);
        if(!status) {
            // A file the build read that is gone already: record the
            // absence, reappearing counts as a change. Still-missing
            // deliberately counts as unchanged — flagging it would rebuild
            // on every check without ever converging, while the artifact is
            // the last remaining truth for the file (and dependents'
            // recovery is the DiskRemoved cascade's job, not this
            // snapshot's).
            dep.missing = true;
            files.disk.saw_missing(dep.path_id);
            continue;
        }

        if(hash == 0) {
            // The worker could not hash the consumed bytes: when nothing
            // else names them, the dep stays version-less and reads as
            // changed until the rebuild's capture retries.
            hash = files.disk.unchanged_hash(dep.path_id, *status, baseline_before_ns);
            if(hash == 0) {
                continue;
            }
        } else {
            files.disk.consumed(dep.path_id, hash);
        }

        dep.version = files.intern_version(dep.path_id, hash);
    }
    return snap;
}

std::optional<DepState> current_dep(FileTable& files, Fid file) {
    if(auto seen = files.disk.current(file)) {
        return DepState{.path_id = file, .version = files.intern_version(file, seen->hash)};
    }
    if(files.disk.seen_missing(file)) {
        return DepState{.path_id = file, .missing = true};
    }
    return std::nullopt;
}

void merge_deps(DepsSnapshot& into, llvm::ArrayRef<const DepsSnapshot*> parts) {
    auto key = [](const DepState& dep) {
        return std::tuple(dep.path_id.raw, dep.version.raw, std::uint8_t(dep.missing));
    };
    llvm::DenseSet<std::tuple<std::uint32_t, std::uint32_t, std::uint8_t>> seen;
    for(auto& dep: into) {
        seen.insert(key(dep));
    }
    for(auto* part: parts) {
        for(auto& dep: *part) {
            if(seen.insert(key(dep)).second) {
                into.push_back(dep);
            }
        }
    }
}

std::shared_ptr<index::TUIndex> load_pch_index(llvm::StringRef path) {
    auto file = fs::File::open(path);
    if(!file) {
        return nullptr;
    }
    auto buffer = llvm::MemoryBuffer::getOpenFile(file->native_handle(),
                                                  path,
                                                  /*FileSize=*/-1,
                                                  /*RequiresNullTerminator=*/false,
                                                  /*IsVolatile=*/false);
    if(!buffer) {
        return nullptr;
    }
    // A stale or truncated pair must never crash the server: the TU index
    // is deep-verified, and every embedded shard blob once — queries then
    // run unchecked. Anything failing reads as "pair missing" and the PCH
    // is rebuilt.
    auto tu_index = index::TUIndex::from_buffer(std::move(*buffer));
    if(!tu_index.loaded() || !tu_index.verify_shards()) {
        return nullptr;
    }
    return std::make_shared<index::TUIndex>(std::move(tu_index));
}

const std::shared_ptr<index::TUIndex>& PCHState::load_state() {
    if(!state && !index_path.empty()) {
        state = load_pch_index(index_path);
        if(!state) {
            // Unreadable blob: clear the path so queries don't retry the
            // mmap + verification on every call. The pair now looks
            // incomplete and ensure_pch rebuilds it on the next compile.
            LOG_WARN("Failed to open pch.idx envelope {}", index_path);
            index_path.clear();
        }
    }
    return state;
}

const DepsSnapshot* Project::build_inputs(Fid module) const {
    llvm::SmallVector<Fid, 4> chain;
    for(auto current = module; !ranges::contains(chain, current);) {
        auto* failed = failures.pcm_builds.find(current);
        if(!failed) {
            auto it = pcm_builds.find(current);
            return it != pcm_builds.end() ? &it->second.deps : nullptr;
        }
        if(!failed->result.import.valid()) {
            return &failed->trace;
        }
        chain.push_back(current);
        current = failed->result.import;
    }
    return nullptr;
}

DepsSnapshot Project::module_inputs(llvm::ArrayRef<Fid> modules) const {
    llvm::SmallVector<const DepsSnapshot*> parts;
    for(auto module: modules) {
        if(auto* inputs = build_inputs(module)) {
            parts.push_back(inputs);
        }
    }
    DepsSnapshot inputs;
    merge_deps(inputs, parts);
    return inputs;
}

void Project::fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                            llvm::ArrayRef<Fid> modules) const {
    for(auto module: modules) {
        auto it = pcm_builds.find(module);
        auto name = dep_graph.module_of(module);
        if(it != pcm_builds.end() && !it->second.path.empty() && !name.empty()) {
            pcms[name.str()] = it->second.path;
        }
    }
}

}  // namespace clice
