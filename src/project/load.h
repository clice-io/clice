#pragma once

#include <string>
#include <vector>

#include "project/index_store.h"
#include "vfs/file_table.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

struct Project;

/// What loading the build found.
struct BuildLoad {
    /// The build's translation units, the dependency graph's roots.
    std::vector<Fid> members;
};

/// Activate the build `configuration` (resolved, see
/// resolve_configuration), register and load the build's sources — the
/// databases the rules declare (existing or not; the tracker watches for
/// them), else the ones discovered under `root` plus `nearby`, the ones
/// discovery would only meet later: the persisted index's (see
/// IndexStore::remembered_sources) for the server, those above the
/// inspected files for `clice inspect` (see compile_commands_above) —
/// then enumerate the build's members and scan the dependency graph from
/// them. The workspace's configuration is final. The one loading path of
/// the server, the batch driver and `clice inspect`.
BuildLoad load_build(Project& project,
                     llvm::StringRef root,
                     llvm::StringRef configuration,
                     llvm::ArrayRef<std::string> nearby = {});

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
    /// indexing sweep's work list, so no driver walks the tree a second
    /// time.
    std::vector<Fid> members;

    /// The persisted index's load: its report names the units the load
    /// found unservable, owed a reindex.
    IndexStore::LoadResult index;
};

/// Load a project from disk: resolve the build configuration
/// (`requested_configuration` is the command line's), open the cache
/// store with its namespaces and the configuration's index library, load
/// the build (load_build) and restore the persisted index. The caller has
/// already finalized project.config; a second call is safe and skips the
/// store and library opens (live CDB reloads go through the invalidator
/// instead).
///
/// `read_only_index` loads the persisted index without queueing any
/// reconciliation or sweep writes, so a later save commits nothing — for
/// runs whose product must not touch the index (plain `clice lint`).
///
/// `scan_tree` makes discovery search the whole tree once
/// (compile_commands_below) instead of waiting for a didOpen: for the
/// batch commands, which open no file.
ProjectLoad load_project(Project& project,
                         IndexStore& store,
                         llvm::StringRef root,
                         llvm::StringRef requested_configuration,
                         bool read_only_index = false,
                         bool scan_tree = false);

}  // namespace clice
