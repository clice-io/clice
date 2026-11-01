module clice:project.discovery;

import :support.prelude;
import :vfs.path;

namespace clice {

/// Whether `dir` holds a configuration file.
bool has_configuration_file(const Spelling& dir);

/// The outermost directory below `project_root` that holds `path`, or is
/// `path`, and has a configuration file: what lies in it is that
/// project's, not `project_root`'s. Empty when there is none.
CanonicalPath nested_project(CanonicalRef project_root, CanonicalRef path);

/// The project root's direct subdirectories, symlinked ones among them —
/// a symlinked build directory is a build directory too, even before its
/// target exists — in name order.
llvm::SmallVector<Spelling> direct_subdirectories(CanonicalRef project_root);

/// Where a `compile_commands.json` is looked for when no rule declares
/// one: the project root, then its direct subdirectories in name order,
/// but for those with a configuration file of their own (nested_project).
llvm::SmallVector<Spelling> database_places(CanonicalRef project_root);

/// The `compile_commands.json` files to load when no rule declares one:
/// those of database_places that exist. Empty when none exists yet — the
/// server keeps watching the places (ServedProject::discover_databases).
llvm::SmallVector<Spelling> discovered_databases(CanonicalRef project_root);

/// The `compile_commands.json` files discovery passes over, deeper than
/// the root's direct subdirectories down to three directories below it
/// (hidden directories, `node_modules`, the cache directory and the
/// directories with a configuration file of their own skipped), by path:
/// what the guidance of a project without a database names.
llvm::SmallVector<Spelling> deeper_databases(CanonicalRef project_root, CanonicalRef cache_dir);

/// Whether `dir` is a project of its own: it holds a configuration file,
/// or a compile_commands.json where startup discovery looks
/// (discovered_databases).
bool defines_project(CanonicalRef dir);

/// The nearest directory at or above `start` holding a configuration file
/// or a compile_commands.json, directly or in its build/ directory — its
/// parent instead when it holds a database alone and the parent is such a
/// directory — spelled as `start` is when `start` lies under it as spelled:
/// the root of the project a directory belongs to, as the server finds it
/// for a file outside every served folder and the command line from the
/// current directory. Empty when no ancestor has either.
Spelling find_project_root(const Spelling& start);

/// Where a `compile_commands.json` is looked for in `start` and its
/// ancestors up to `project_root`, nearest first: none in or below a
/// directory with a configuration file of its own (nested_project).
llvm::SmallVector<Spelling> database_places_above(CanonicalRef start, CanonicalRef project_root);

/// The `compile_commands.json` files of database_places_above that exist:
/// the databases a file deeper in the tree than startup discovery looks
/// may compile from.
llvm::SmallVector<Spelling> databases_above(CanonicalRef start, CanonicalRef project_root);

/// How discovered databases rank among themselves: shallower before
/// deeper, then by path.
std::pair<std::uint32_t, llvm::StringRef> discovery_rank(llvm::StringRef database);

/// Whether `file` is the project's own — what a refactoring may edit and
/// a save may record as a provisional member: it lies under `root`,
/// whatever its suffix, outside the directories holding no sources a
/// refactoring edits — hidden ones, package caches, `cache_dir` and build
/// trees (a directory holding CMakeCache.txt or build.ninja).
bool project_file(CanonicalRef root, CanonicalRef cache_dir, CanonicalRef file);

/// What a refactoring may edit of the project at `root`, outside its cache
/// directory (feature::RenameScope).
struct EditableFiles {
    /// The C-family sources and headers of the project, as absolute
    /// paths: the files under `root` a rename's scan reads, outside the
    /// directories project_file leaves out.
    std::vector<std::string> sources() const;

    /// project_file of an absolute path.
    bool editable(llvm::StringRef path) const;

    CanonicalPath root;

    CanonicalPath cache_dir;
};

}  // namespace clice
