#pragma once

#include <string>

#include "command/command.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class ContextResolver;
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
};

/// The one workspace loading sequence, shared by the server's initialize
/// and the batch driver so the two can never drift apart: open the cache
/// store and register its namespaces, discover and load
/// the CDB, scan the dependency graph and build the module map, restore
/// the persisted index (claiming its report into the pump), and seed the
/// indexing sweep. The caller has already finalized workspace.config; a
/// second call is safe and skips the store open (live CDB reloads go
/// through the invalidator instead).
///
/// `read_only_index` loads the persisted index without queueing any
/// reconciliation or sweep writes, so a later save commits nothing — for
/// runs whose product must not touch the index (plain `clice lint`).
BootstrapReport bootstrap_workspace(Workspace& workspace,
                                    ContextResolver& contexts,
                                    IndexStore& store,
                                    IndexPump& pump,
                                    llvm::StringRef root,
                                    bool read_only_index = false);

/// The active view's members with their effective commands: the input of
/// a dependency scan. Shared by the startup scan and the CDB reload
/// rescan so the two can never disagree on what a unit compiles as.
llvm::SmallVector<CommandRef> scan_units(Workspace& workspace);

}  // namespace clice
