#pragma once

#include <cstdint>
#include <string>

#include "project/cdb_watcher.h"
#include "project/project.h"
#include "server/state/invalidator.h"
#include "server/state/session_store.h"

#include "kota/async/async.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

/// Stat-based discovery of changes the client never tells us about:
/// compile_commands.json edits (its CDBWatcher) and files
/// changing on disk behind the server's back (git checkout, code
/// generators, save hooks), swept here.
///
/// Core design property: polling only marks dirty and emits events — it
/// never needs to be complete. A missed change means derived state stays
/// stale for one more poll period at worst; correctness is anchored by the
/// pull side's two-layer DepsSnapshot validation (mtime, then content hash)
/// at compile and index time. That is what lets this implementation stay
/// simple and coarse, and why it polls stat instead of using inotify — for
/// clangd's reasons: portable, no fd limits, no event storms.
///
/// The tracker only observes and returns event batches; it never
/// dispatches. MasterServer's polling loops (and the clice/internal/poll
/// test hook) hand each batch to dispatch(), which keeps the tracker
/// unit-testable against plain data structures.
class FileTracker {
public:
    /// Construct after the project is loaded: its databases are baselined
    /// at their loads.
    FileTracker(Project& project, const SessionStore& store, std::string root);

    /// One CDB poll tick (see CDBWatcher::tick), the open files looking
    /// for a database; the reload's diff as one CDBChanged event.
    llvm::SmallVector<FileEvent> tick_cdb(bool force = false);

    /// See CDBWatcher::discover_around; the loads' diffs as CDBChanged
    /// events.
    llvm::SmallVector<FileEvent> discover_around(Fid path_id);

    /// One workspace sweep. Stats every file the dependency graph knows,
    /// skipping open buffers; a (mtime, size) suspect is confirmed by
    /// content hash before DiskChanged is emitted, so touch-only changes
    /// (mtime bump, identical bytes) stay silent. A stat failure on a
    /// known file emits DiskRemoved once; a transient content-read failure
    /// emits nothing and is retried on the next tick.
    ///
    /// A file is judged against the content its include edges were scanned
    /// from when first seen — at construction for the load's scan, so a
    /// change landing before the first sweep is still reported — and again
    /// when it closes, so a file deleted or changed while open without a
    /// cascade is reported then. A file no scan read only seeds the
    /// baseline.
    ///
    /// Stats run synchronously in batches, yielding to the event loop
    /// between batches; each round's duration is perf-logged.
    /// TODO: offload stats to the thread pool (and consider a directory
    /// listing cache for Windows, where per-file stat is expensive) if the
    /// logged sweep timing shows the need.
    kota::task<llvm::SmallVector<FileEvent>> tick_workspace();

private:
    /// Last-known on-disk state of a tracked file.
    struct FileState {
        std::uint64_t size = 0;
        std::int64_t mtime_ns = 0;
        std::uint64_t hash = 0;
        std::uint64_t uid_device = 0;
        std::uint64_t uid_file = 0;
        bool missing = false;
    };

    Project& project;
    const SessionStore& store;
    CDBWatcher cdb;

    /// Workspace sweep baseline.
    llvm::DenseMap<Fid, FileState> baseline;

    /// True while a sweep is in flight (it suspends between batches);
    /// concurrent ticks are skipped instead of racing on the baseline.
    bool sweeping = false;
};

}  // namespace clice
