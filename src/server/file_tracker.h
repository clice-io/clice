#pragma once

#include <cstdint>
#include <string>

#include "project/cdb_watcher.h"
#include "project/project.h"
#include "server/invalidator.h"
#include "server/session_store.h"

#include "kota/async/async.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

/// Stat-based discovery of changes the client never tells us about:
/// compile_commands.json edits (its CDBWatcher) and files
/// changing on disk behind the server's back (git checkout, code
/// generators, save hooks), looked at here.
///
/// Core design property: polling only marks dirty and emits events — it
/// never needs to be complete. A missed change means derived state stays
/// stale for one more poll period at worst; correctness is anchored by the
/// pull side's two-layer DepsSnapshot validation (mtime, then content hash)
/// at compile and index time. That is what lets this implementation stay
/// simple and coarse, and why it polls stat instead of using inotify — for
/// clangd's reasons: portable, no fd limits, no event storms.
///
/// The tracker only observes; it never dispatches. Disk changes surface
/// through the file table's change queue, database changes as the
/// returned batches the polling loops (and the clice/internal/poll test
/// hook) hand to dispatch(), which keeps the tracker unit-testable
/// against plain data structures.
class FileTracker {
public:
    /// Construct after the project is loaded: its databases are baselined
    /// at their loads.
    FileTracker(Project& project, const SessionStore& store, CanonicalPath root);

    /// One CDB poll tick (see CDBWatcher::tick), the open files looking
    /// for a database; the reload's diff as one CDBChanged event.
    llvm::SmallVector<FileEvent> tick_cdb(bool force = false);

    /// See CDBWatcher::discover_around; the loads' diffs as CDBChanged
    /// events.
    llvm::SmallVector<FileEvent> discover_around(Fid path_id);

    /// One workspace sweep: look at every file the dependency graph knows
    /// or an indexed compile read — open ones included, a buffer shadows
    /// the disk only for its own file's compile — and every place the file
    /// table last saw empty, through the file table, which turns every look
    /// that finds other content than it last saw into a change (see
    /// FileTable::changes); the sweep itself keeps no state. An unchanged
    /// file costs one stat the shared pair vouches for, a moved stat one
    /// read, so touch-only changes stay silent. Returns the build's gain of
    /// default-command sources as a CDBChanged event.
    ///
    /// Stats run synchronously in batches, yielding to the event loop
    /// between batches; each round's duration is perf-logged.
    /// TODO: offload stats to the thread pool (and consider a directory
    /// listing cache for Windows, where per-file stat is expensive) if the
    /// logged sweep timing shows the need.
    kota::task<llvm::SmallVector<FileEvent>> tick_workspace();

private:
    Project& project;
    const SessionStore& store;
    CDBWatcher cdb;

    /// True while a sweep is in flight (it suspends between batches);
    /// concurrent ticks are skipped.
    bool sweeping = false;
};

}  // namespace clice
