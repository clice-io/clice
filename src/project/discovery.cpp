module clice;

import :command.database;
import :command.file_kind;
import :project.discovery;
import :project.project;
import :settings.settings;
import :support.logging;
import :vfs.file_system;
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

/// Whether a project directory holds no sources a refactoring edits:
/// hidden, a package cache, the cache directory or a build tree.
static bool outside_sources(CanonicalRef dir, CanonicalRef cache_dir) {
    auto name = path::filename(dir);
    return name.starts_with(".") || name == "node_modules" || dir == cache_dir ||
           fs::exists(path::join(dir, "CMakeCache.txt")) ||
           fs::exists(path::join(dir, "build.ninja"));
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

}  // namespace clice
