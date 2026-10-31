module clice:project.discovery;

import :support.prelude;
import :vfs.path;

namespace clice {

struct Project;

/// Whether `dir` holds a configuration file.
bool configured(const Spelling& dir);

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
llvm::SmallVector<Spelling> discover_compile_commands(CanonicalRef project_root);

/// The `compile_commands.json` files discovery passes over, deeper than
/// the root's direct subdirectories down to three directories below it
/// (hidden directories, `node_modules`, the cache directory and the
/// directories with a configuration file of their own skipped), by path:
/// what the guidance of a project without a database names.
llvm::SmallVector<Spelling> deeper_compile_commands(CanonicalRef project_root,
                                                    CanonicalRef cache_dir);

/// Whether `dir` is a project of its own: it holds a configuration file,
/// or a compile_commands.json where startup discovery looks
/// (discover_compile_commands).
bool defines_project(CanonicalRef dir);

/// The nearest directory at or above `start` holding a configuration file
/// or a compile_commands.json, directly or in its build/ directory — its
/// parent instead when it holds a database alone and the parent is such a
/// directory — spelled as `start` is when `start` lies under it as spelled:
/// the root of the project a directory belongs to, as the server finds it
/// for a file outside every served folder and the command line from the
/// current directory. Empty when no ancestor has either.
Spelling find_project_root(const Spelling& start);

/// The sentence the guidance of a project without a database ends with:
/// the rule that loads the compile_commands.json files discovery passes
/// over (deeper_compile_commands). Empty when there is none, or the rules
/// declare the build's sources.
std::string deeper_databases_hint(const Project& project);

/// Whether a database the load found on disk could not be read, which its
/// load logged the reason for.
bool found_unreadable_database(const Project& project);

/// Why the build has no member, completing a batch command's "Nothing to
/// <verb>: ": a database it found that cannot be read, does not exist yet
/// or lists no file; the rules' default commands claiming no file; else
/// that no database was found in `root` or its direct subdirectories, the
/// subdirectories left out as projects of their own, and the deeper ones
/// there are.
std::string why_no_members(const Project& project, llvm::StringRef root);

/// Where a `compile_commands.json` is looked for in `start` and its
/// ancestors up to `project_root`, nearest first: none in or below a
/// directory with a configuration file of its own (nested_project).
llvm::SmallVector<Spelling> database_places_above(CanonicalRef start, CanonicalRef project_root);

/// The `compile_commands.json` files of database_places_above that exist:
/// the databases a file deeper in the tree than startup discovery looks
/// may compile from.
llvm::SmallVector<Spelling> compile_commands_above(CanonicalRef start, CanonicalRef project_root);

}  // namespace clice
