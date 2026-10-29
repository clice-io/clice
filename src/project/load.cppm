module clice:project.load;

import :index.project_index;
import :project.project;
import :support.prelude;
import :syntax.dependency_graph;
import :vfs.file_table;

namespace clice {

/// What loading the build found.
struct BuildLoad {
    /// The build's translation units, the dependency graph's roots.
    std::vector<Fid> members;

    /// The dependency scan from them.
    ScanReport scan;
};

/// Activate the build `configuration` (resolved, see
/// resolve_configuration) and register and load the build's sources: the
/// databases the rules declare (existing or not; the tracker watches for
/// them), else the ones discovered under `root` plus `nearby`, the ones
/// discovery would only meet later — the persisted index's (see
/// CommandSnapshot::remembered) for the server, those above the files a
/// command names (see compile_commands_above). The project's configuration
/// is final.
void load_sources(Project& project,
                  CanonicalRef root,
                  llvm::StringRef configuration,
                  llvm::ArrayRef<Spelling> nearby);

/// load_sources, then record the `provisional` members the persisted index
/// remembers, enumerate the build's members and scan the dependency graph
/// from them. The one loading path of the server, the batch driver and
/// `clice inspect`.
BuildLoad load_build(Project& project,
                     CanonicalRef root,
                     llvm::StringRef configuration,
                     llvm::ArrayRef<Spelling> nearby = {},
                     llvm::ArrayRef<Spelling> provisional = {});

/// The sentence the guidance of a workspace without a database ends with:
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
/// that no database was found in `root` or its direct subdirectories, and
/// the deeper ones there are.
std::string why_no_members(const Project& project, llvm::StringRef root);

/// What load_saved found.
struct SavedLoad {
    /// False when the database's header is of another format or corrupt:
    /// the server discards the database and starts over, but a read-only
    /// reader must report an unusable cache instead of an empty index.
    bool decoded = true;

    /// The units owed a reindex the load found: the ones a rule keeps out or
    /// whose command changed while no server ran.
    index::IndexReport report;
};

/// Open what the project's database holds: the index (ProjectIndex::open),
/// the artifact records into the project and `commands`, the context
/// choices; then, unless read-only, drop the index of the units a rule
/// keeps out and of those whose command changed while no server ran
/// (CommandSnapshot::reconcile). A database of another format or found
/// corrupt is replaced by a fresh one; one whose header cannot be read is
/// left untouched, and persistence is off for the session.
SavedLoad load_saved(Project& project);

/// What load_project found and did.
struct ProjectLoad {
    /// Whether any compile command source is in place: a database loaded,
    /// or a rule's default command. False means every file compiles with
    /// the builtin fallback until a database appears — the persisted
    /// index is still loaded then, so a database generated later starts
    /// from the previous session's state.
    bool has_commands = false;

    /// This call opened the cache store: the caller owns store-lifetime
    /// services (the server spawns its checkpoint task on this).
    bool opened_store = false;

    /// The build's translation units as load_build enumerated them: the
    /// indexing pass's work list, so no driver walks the tree a second
    /// time.
    std::vector<Fid> members;

    /// The persisted index's load.
    SavedLoad index;
};

/// Load a project from disk: resolve the build configuration
/// (`requested_configuration` is the command line's), open the cache
/// store with its namespaces and the configuration's index database, load
/// the build (load_build) and restore the persisted index. The caller has
/// already finalized project.config; a second call is safe and skips the
/// store and database opens (live CDB reloads go through the invalidator
/// instead).
///
/// `read_only_index` loads the persisted index without queueing any
/// reconciliation or removal writes, so a later save commits nothing — for
/// runs whose product must not touch the index (plain `clice lint`).
ProjectLoad load_project(Project& project,
                         CanonicalRef root,
                         llvm::StringRef requested_configuration,
                         bool read_only_index = false);

}  // namespace clice
