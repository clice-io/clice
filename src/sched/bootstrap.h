#pragma once

#include <string>
#include <vector>

#include "vfs/file_table.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class IndexPump;
class IndexStore;
struct Workspace;

/// What bootstrap_workspace found and did, for the caller's own follow-ups
/// (guidance messages, store-lifetime services).
struct BootstrapReport {
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
    /// batch drivers' work list, so they never walk the tree a second time.
    std::vector<Fid> members;
};

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
BuildLoad load_build(Workspace& workspace,
                     llvm::StringRef root,
                     llvm::StringRef configuration,
                     llvm::ArrayRef<std::string> nearby = {});

/// The one workspace loading sequence, shared by the server's initialize
/// and the batch driver so the two can never drift apart: resolve the
/// build configuration (`requested_configuration` is the command line's),
/// open the cache store with its namespaces and the configuration's index
/// library, load the build (load_build), restore the persisted index
/// (claiming its report into the pump), and seed the indexing sweep. The
/// caller has already finalized workspace.config; a second call is safe
/// and skips the store and library opens (live CDB reloads go through the
/// invalidator instead).
///
/// `read_only_index` loads the persisted index without queueing any
/// reconciliation or sweep writes, so a later save commits nothing — for
/// runs whose product must not touch the index (plain `clice lint`).
///
/// `nearby` joins the persisted index's remembered databases as the ones
/// discovery would only meet later (see load_build): the batch commands
/// pass what compile_commands_below finds.
BootstrapReport bootstrap_workspace(Workspace& workspace,
                                    IndexStore& store,
                                    IndexPump& pump,
                                    llvm::StringRef root,
                                    llvm::StringRef requested_configuration,
                                    bool read_only_index = false,
                                    llvm::ArrayRef<std::string> nearby = {});

}  // namespace clice
