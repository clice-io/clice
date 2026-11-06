module clice;

import :command.database;
import :command.file_kind;
import :project.discovery;
import :project.project;
import :settings.settings;
import :support.logging;
import :vfs.disk_state;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;

namespace clice {

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

/// Whether a project directory holds no sources a refactoring edits:
/// hidden, a package cache, the cache directory or a build tree.
bool outside_sources(CanonicalRef dir, CanonicalRef cache_dir) {
    auto name = path::filename(dir);
    return name.starts_with(".") || name == "node_modules" || dir == cache_dir ||
           fs::exists(path::join(dir, "CMakeCache.txt")) ||
           fs::exists(path::join(dir, "build.ninja"));
}

/// The git directory of the checkout `root` lies in, if any: `.git`
/// itself, or the directory a linked worktree's `.git` file names.
std::optional<std::string> git_dir(llvm::StringRef root) {
    for(auto dir: path::ancestors(root)) {
        auto git = path::join(dir, ".git");
        auto status = fs::status(git);
        if(!status) {
            continue;
        }
        if(status->type == fs::FileType::Directory) {
            return git;
        }
        if(auto text = vfs::read_text(git)) {
            llvm::StringRef target = *text;
            if(target.consume_front("gitdir:")) {
                return Spelling(target.trim(), Spelling::absolute(dir)).str();
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

/// The hashes of the database's last load.
InputHashes loaded_hashes(const Project& project, SourceID id) {
    return ranges::to<InputHashes>(
        views::transform(project.cdb.inputs(id), [](auto& input) { return input.hash; }));
}

/// Watch the response files the database's last load read, besides the
/// database.
void watch_inputs(Project& project, WatchedDatabase& database) {
    database.inputs.truncate(1);
    for(auto& input: project.cdb.inputs(database.id).drop_front()) {
        database.inputs.push_back(
            project.file_table.disk.watch(project.file_table.resolve(input.file).str()));
    }
}

/// Watch the database from its last load.
void watch_database(Project& project, SourceID id) {
    auto& database = project.disk_watch.databases.emplace_back(WatchedDatabase{
        .id = id,
        .applied = loaded_hashes(project, id),
        .inputs = {project.file_table.disk.watch(project.cdb.source_path(id).str())},
    });
    watch_inputs(project, database);
}

/// The files the database and another one both list: the ones whose
/// default entry may move with the database's presence.
llvm::SmallVector<Fid> shared_files(const Project& project, SourceID id) {
    llvm::SmallVector<Fid> shared;
    for(auto group: project.cdb.entries() |
                        views::chunk_by([](const CompilationEntry& a, const CompilationEntry& b) {
                            return a.file == b.file;
                        })) {
        auto listed = [&](const CompilationEntry& entry) {
            return entry.source == id;
        };
        if(ranges::any_of(group, listed) && !ranges::all_of(group, listed)) {
            shared.push_back(group.front().file);
        }
    }
    return shared;
}

/// The source of each file's default entry, as the build ranks them now;
/// none for a file the build no longer compiles.
llvm::SmallVector<std::optional<SourceID>> default_sources(const Project& project,
                                                           llvm::ArrayRef<Fid> files) {
    return ranges::to<llvm::SmallVector<std::optional<SourceID>>>(
        views::transform(files, [&](Fid file) -> std::optional<SourceID> {
            auto entries = project.entries(file);
            if(entries.empty()) {
                return std::nullopt;
            }
            return entries.front().source;
        }));
}

/// The files whose default entry moved between two rankings, into
/// `changed`.
void push_moved(llvm::ArrayRef<Fid> files,
                llvm::ArrayRef<std::optional<SourceID>> before,
                llvm::ArrayRef<std::optional<SourceID>> after,
                llvm::SmallVectorImpl<Fid>& changed) {
    for(auto [file, was, now]: views::zip(files, before, after)) {
        if(was != now && !ranges::contains(changed, file)) {
            changed.push_back(file);
        }
    }
}

/// One database's look, its reload's delta merged into `delta`. Once its
/// inputs have held the same other content for two consecutive looks (or
/// `force`), it reloads.
void look_at_database(Project& project, WatchedDatabase& database, bool force, CDBDiff& delta) {
    auto& cdb = project.cdb;
    auto current = ranges::to<InputHashes>(
        views::transform(database.inputs, [](auto& input) { return input->hash; }));
    if(!force) {
        if(current == database.applied) {
            database.pending.reset();
            return;
        }
        if(database.pending != current) {
            // Generators rewrite the file in place; only act once the
            // content has held for two consecutive ticks (half-write guard).
            database.pending = std::move(current);
            return;
        }
    }
    // A forced tick reloads unconditionally: a spurious reload just yields
    // an empty diff.
    database.pending.reset();
    bool exists = database.inputs.front()->stamp.has_value();
    // A discovered database's presence ranks it (see Project::source_order):
    // the files whose default entry moves with it change command.
    bool flips = cdb.present(database.id) != exists && project.discovered(database.id);
    llvm::SmallVector<Fid> shared;
    llvm::SmallVector<std::optional<SourceID>> before;
    if(flips) {
        shared = shared_files(project, database.id);
        before = default_sources(project, shared);
    }
    if(!exists) {
        // Deleted — usually mid-regeneration. Keep serving the loaded
        // entries; the rewrite arrives as the next observed change.
        database.applied = std::move(current);
        cdb.set_present(database.id, false);
        if(flips) {
            push_moved(shared, before, default_sources(project, shared), delta.changed);
        }
        return;
    }
    auto diff = cdb.reload_and_diff(database.id);
    // The baseline is the reload's own reads: a rewrite arriving meanwhile
    // is seen next tick. A database the reload could not read (still
    // locked by the generator) leaves them as they were, so the reload is
    // retried; one it read but could not parse is retried once it moves.
    database.applied = loaded_hashes(project, database.id);
    watch_inputs(project, database);
    if(!diff) {
        return;
    }
    LOG_INFO("Reloaded CDB from {}: {} added, {} removed, {} changed",
             cdb.source_path(database.id),
             diff->added.size(),
             diff->removed.size(),
             diff->changed.size());
    if(flips) {
        push_moved(shared, before, default_sources(project, shared), diff->changed);
    }
    delta.append(*diff);
}

}  // namespace

bool has_configuration_file(const Spelling& dir) {
    return ranges::any_of(settings_file_names,
                          [&](llvm::StringRef name) { return fs::exists(Spelling(name, dir)); });
}

CanonicalPath nested_project(CanonicalRef project_root, CanonicalRef path) {
    CanonicalPath found;
    if(project_root.empty() || !path::is_within(path, project_root)) {
        return found;
    }
    path::walk_ancestors(path, [&](CanonicalRef dir) {
        if(dir == project_root) {
            return false;
        }
        if(has_configuration_file(Spelling(dir))) {
            found = dir;
        }
        return true;
    });
    return found;
}

llvm::SmallVector<Spelling> direct_subdirectories(CanonicalRef project_root) {
    llvm::SmallVector<Spelling> subdirectories;
    if(auto entries = fs::Directory::open(project_root)) {
        for(auto& entry: *entries) {
            if(!entry) {
                break;
            }
            if(entry->type == fs::FileType::Symlink || entry->type == fs::FileType::Directory) {
                subdirectories.push_back(Spelling::absolute(path::join(project_root, entry->name)));
            }
        }
    }
    // Name order, so build/ and out/ side by side load in the same order on
    // every start rather than whichever the directory listing yields first.
    ranges::sort(subdirectories);
    return subdirectories;
}

llvm::SmallVector<Spelling> database_places(CanonicalRef project_root) {
    llvm::SmallVector<Spelling> places;
    if(project_root.empty()) {
        return places;
    }
    places.emplace_back(database_name, Spelling(project_root));
    for(auto& subdirectory: direct_subdirectories(project_root)) {
        if(!has_configuration_file(subdirectory)) {
            places.emplace_back(database_name, subdirectory);
        }
    }
    return places;
}

llvm::SmallVector<Spelling> discovered_databases(CanonicalRef project_root) {
    return existing(database_places(project_root));
}

llvm::SmallVector<Spelling> deeper_databases(CanonicalRef project_root, CanonicalRef cache_dir) {
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
               project_root.entry(spelled) == cache_dir || has_configuration_file(spelled)) {
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
    return has_configuration_file(Spelling(dir)) || !discovered_databases(dir).empty();
}

Spelling find_project_root(const Spelling& start) {
    // The directories holding `start`, not the links on the way to it: a
    // directory reached through a symlink belongs to the project of the one
    // it links to.
    auto defines = [](const Spelling& dir) {
        return has_configuration_file(dir) || database_in(dir) ||
               database_in(Spelling("build", dir));
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
        if(!has_configuration_file(spelled) && !database_in(Spelling("build", spelled)) &&
           !parent.empty() && defines(Spelling(parent))) {
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

std::string Project::deeper_databases_hint() const {
    if(rules.declares_sources()) {
        return {};
    }
    CanonicalRef root = this->root;
    std::vector<std::string> names;
    for(auto& database: deeper_databases(root, settings.project.cache_identity)) {
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

bool Project::found_unreadable_database() const {
    return ranges::any_of(views::iota(std::size_t(0), cdb.source_count()), [&](std::size_t i) {
        return !cdb.loaded(SourceID(i)) && fs::exists(cdb.source_path(SourceID(i)));
    });
}

std::string Project::why_no_members(llvm::StringRef root) const {
    std::vector<std::string> empty;
    for(std::size_t i = 0; i < cdb.source_count(); i += 1) {
        auto path = cdb.source_path(SourceID(i));
        if(cdb.loaded(SourceID(i))) {
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
    if(rules.declares_sources()) {
        return "no file on disk matches a rule with a default_command.";
    }
    std::vector<std::string> nested;
    for(auto& subdirectory: direct_subdirectories(CanonicalPath(Spelling::absolute(root)))) {
        if(has_configuration_file(subdirectory)) {
            nested.emplace_back(path::filename(subdirectory));
        }
    }
    std::string projects;
    if(nested.size() == 1) {
        projects = std::format(" {} has its own configuration file and is a project of its own.",
                               nested.front());
    } else if(!nested.empty()) {
        projects =
            std::format(" {} have their own configuration files and are projects of their own.",
                        llvm::join(nested, ", "));
    }
    return std::format("no compile_commands.json found in {} or its direct subdirectories.{}{}",
                       root.str(),
                       projects,
                       deeper_databases_hint());
}

llvm::SmallVector<Spelling> database_places_above(CanonicalRef start, CanonicalRef project_root) {
    llvm::SmallVector<Spelling> places;
    for(auto dir: path::ancestors(start)) {
        auto spelled = Spelling::absolute(dir);
        bool root = dir == std::string_view(project_root);
        // A directory with a configuration file of its own and everything
        // below it are that project's.
        if(!root && has_configuration_file(spelled)) {
            places.clear();
        } else {
            places.emplace_back(database_name, std::move(spelled));
        }
        if(root) {
            break;
        }
    }
    return places;
}

llvm::SmallVector<Spelling> databases_above(CanonicalRef start, CanonicalRef project_root) {
    return existing(database_places_above(start, project_root));
}

std::pair<std::uint32_t, llvm::StringRef> discovery_rank(llvm::StringRef database) {
    auto depth = ranges::count_if(database, [](char c) { return path::is_separator(c); });
    return {static_cast<std::uint32_t>(depth), database};
}

bool project_file(CanonicalRef root, CanonicalRef cache_dir, CanonicalRef file) {
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

EditableFiles Project::editable_files() const {
    return {.root = root, .cache_dir = settings.project.cache_identity};
}

std::vector<std::string> EditableFiles::sources() const {
    CanonicalRef project_root = root;
    std::vector<std::string> found;
    auto entries = fs::walk(project_root);
    for(auto& entry: entries) {
        if(!entry) {
            vfs::warn_unreadable(entry.error());
            continue;
        }
        if(entry->type == fs::FileType::Directory) {
            if(outside_sources(project_root.entry(entry->path), cache_dir)) {
                entries.skip();
            }
            continue;
        }
        if(entry->type != fs::FileType::Regular) {
            continue;
        }
        auto path = project_root.entry(entry->path);
        if(clang::driver::types::isAcceptedByClang(suffix_type(path)) ||
           is_context_header_path(path) || path.str().ends_with(".cuh")) {
            found.push_back(std::move(path).str());
        }
    }
    return found;
}

bool EditableFiles::editable(llvm::StringRef path) const {
    return project_file(root, cache_dir, CanonicalPath(Spelling::absolute(path)));
}


bool Project::databases_moved() const {
    return rules.resolved_databases() != resolved_databases;
}

void Project::watch_databases() {
    for(std::size_t i = 0; i < cdb.source_count(); i += 1) {
        watch_database(*this, SourceID(i));
    }
    auto git = git_dir(root);
    if(!git) {
        return;
    }
    for(auto name: {"HEAD", "index"}) {
        disk_watch.checkout.push_back(file_table.disk.watch(
            path::join(*git, name),
            [&disk = file_table.disk, root = root.str()] { disk.expire_under(root); }));
    }
}

CDBDiff Project::look_at_databases(bool force, llvm::ArrayRef<Fid> open) {
    discover_databases(open, false);
    CDBDiff delta;
    for(auto& database: disk_watch.databases) {
        look_at_database(*this, database, force, delta);
    }
    return delta;
}

CDBDiff Project::discover_databases(llvm::ArrayRef<Fid> files, bool now) {
    CDBDiff found;
    if(rules.declares_sources()) {
        return found;
    }
    auto& disk = file_table.disk;
    auto& watch = disk_watch;

    // In the order a startup discovery would register them, the nearest
    // database first above each file.
    llvm::SmallVector<Spelling> wanted;
    llvm::StringSet<> seen;
    auto want = [&](llvm::ArrayRef<Spelling> candidates) {
        for(auto& place: candidates) {
            if(seen.insert(place).second) {
                wanted.push_back(place);
            }
        }
    };
    if(!now) {
        if(!watch.root) {
            watch.root = disk.watch(root.str());
        }
        if(!watch.listed_at || watch.root->stamp != watch.listed_at) {
            watch.listed = database_places(root);
            // An entry made within the clock tick of the root's last change
            // leaves its stamp as it was: only a stable stamp guarantees the
            // listing (see vfs::stable_mtime).
            auto& stamp = watch.root->stamp;
            watch.listed_at = stamp && vfs::stable_mtime(stamp->mtime_ns) ? stamp : std::nullopt;
        }
        want(watch.listed);
    }
    for(auto file: files) {
        auto path = file_table.resolve(file);
        if(entries(file).empty() && path::is_within(path, root)) {
            want(database_places_above(path.parent(), root));
        }
    }
    if(!now) {
        llvm::SmallVector<std::string> unwanted;
        for(auto& entry: watch.places) {
            if(!seen.contains(entry.getKey())) {
                unwanted.push_back(entry.getKey().str());
            }
        }
        for(auto& place: unwanted) {
            watch.places.erase(place);
        }
    }

    auto watched = [&](const Spelling& place) -> const vfs::Flag* {
        auto& flag = watch.places[place];
        if(!flag) {
            flag = disk.watch(place.str());
        }
        return flag.get();
    };
    for(auto& place: wanted) {
        auto registered = cdb.find_source(place);
        if(registered && (!now || cdb.loaded(*registered))) {
            continue;
        }
        // A load at once looks at the disk itself: a watched place's flag
        // tells what the last tick saw, and with polling off no tick looks.
        if(now ? !fs::exists(place) : !watched(place)->stamp) {
            continue;
        }
        // Another spelling of a tracked database, as build/compile_commands.json
        // is when the root links it, stays watched: once the link goes, it
        // is a database of its own.
        if(!registered && cdb.find_same_file(place)) {
            continue;
        }
        watch.places.erase(place);
        auto id = registered ? *registered : cdb.add_source(place);
        // Unless loaded now, it is baselined unread: the fresh file is a
        // change against the never-loaded source and goes through the
        // normal debounce-and-reload path.
        if(now) {
            if(auto diff = cdb.reload_and_diff(id)) {
                found.append(*diff);
            }
        }
        if(!registered) {
            LOG_INFO("Found compilation database: {}", place);
            watch_database(*this, id);
        }
    }
    return found;
}

std::vector<CanonicalPath> Project::configuration_dirs(llvm::ArrayRef<Fid> open) const {
    std::vector<CanonicalPath> dirs;
    CanonicalRef root = this->root;
    if(root.empty()) {
        return dirs;
    }
    for(auto& subdirectory: direct_subdirectories(root)) {
        dirs.emplace_back(subdirectory);
    }
    auto down_to = [&](CanonicalRef path) {
        path::walk_ancestors(path.parent(), [&](CanonicalRef dir) {
            if(dir == root || !path::is_within(dir, root)) {
                return false;
            }
            dirs.emplace_back(dir);
            return true;
        });
    };
    for(std::uint32_t i = 0; i < cdb.source_count(); i += 1) {
        down_to(CanonicalPath(Spelling::absolute(cdb.source_path(SourceID(i)))));
    }
    for(auto file: provisional.files()) {
        down_to(file_table.resolve(file));
    }
    for(auto file: open) {
        down_to(file_table.resolve(file));
    }
    ranges::sort(dirs);
    dirs.erase(ranges::unique(dirs).begin(), dirs.end());
    return dirs;
}

}  // namespace clice
