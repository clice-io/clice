#pragma once

#include <cstdint>
#include <string>

#include "sched/workspace.h"
#include "server/state/invalidator.h"
#include "server/state/session_store.h"

#include "kota/async/async.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

/// Stat-based discovery of changes the client never tells us about:
/// compile_commands.json edits and files changing on disk behind the
/// server's back (git checkout, code generators, save hooks).
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
    /// Records the stamp every registered database source corresponds to.
    /// Construct after the workspace is loaded.
    FileTracker(Workspace& workspace, const SessionStore& store, std::string workspace_root);

    /// One CDB poll tick. Stats every registered source — declared ones
    /// that do not exist yet included, which is how a database generated
    /// after startup is picked up — and, when no rule declares a source
    /// and none was discovered yet, keeps discovering one. Once a source's
    /// (size, mtime) change has stayed stable for two consecutive ticks,
    /// reloads it and emits one CDBChanged event carrying the reload's
    /// diff.
    ///
    /// `force` reloads unconditionally: it skips both the (size, mtime)
    /// stamp gate — which could hide a same-size rewrite landing within
    /// mtime granularity — and the two-tick settling debounce (the
    /// half-written-file guard). The test hook uses it so a single poll
    /// request applies a change deterministically; a spurious forced
    /// reload just yields an empty diff.
    llvm::SmallVector<FileEvent> tick_cdb(bool force = false);

    /// One workspace sweep. Stats every file the dependency graph knows,
    /// skipping open buffers; a (mtime, size) suspect is confirmed by
    /// content hash before DiskChanged is emitted, so touch-only changes
    /// (mtime bump, identical bytes) stay silent. A stat failure on a
    /// known file emits DiskRemoved once; a transient content-read failure
    /// emits nothing and is retried on the next tick.
    ///
    /// Files seen for the first time only seed the baseline and emit
    /// nothing — the first sweep after startup is silent by construction
    /// (startup storm guard), and files entering the graph later start
    /// tracking silently too.
    ///
    /// Stats run synchronously in batches, yielding to the event loop
    /// between batches; each round's duration is perf-logged.
    /// TODO: offload stats to the thread pool (and consider a directory
    /// listing cache for Windows, where per-file stat is expensive) if the
    /// logged sweep timing shows the need.
    kota::task<llvm::SmallVector<FileEvent>> tick_workspace();

private:
    /// (existence, size, mtime) identity of a database file.
    struct CDBStamp {
        bool exists = false;
        std::uint64_t size = 0;
        std::int64_t mtime_ns = 0;

        friend bool operator==(const CDBStamp&, const CDBStamp&) = default;
    };

    static CDBStamp stat_cdb(llvm::StringRef path);

    /// One registered source's watch state.
    struct TrackedSource {
        SourceID id;
        /// The stamp the loaded entries correspond to.
        CDBStamp applied;
        /// Debounce: the stamp observed on the previous tick, not yet settled.
        CDBStamp pending;
        bool has_pending = false;
        /// The vanished discovered databases this one replaces: they keep
        /// serving their entries until this one has loaded.
        llvm::SmallVector<SourceID, 1> supersedes;
    };

    /// Register `id` for watching, baselined at its current stamp.
    void track(SourceID id);

    /// Tick one source; the reload's events, if any.
    /// Returns the sources this one superseded once its own reload landed:
    /// the caller drops them from the watch list.
    llvm::SmallVector<SourceID, 1> tick_source(TrackedSource& tracked,
                                               bool force,
                                               llvm::SmallVectorImpl<FileEvent>& events);

    /// Last-known on-disk state of a tracked file. The filesystem
    /// identity is part of the stamp: a rename-over with a forged equal
    /// size and mtime still changes the UniqueID.
    struct FileState {
        std::uint64_t size = 0;
        std::int64_t mtime_ns = 0;
        std::uint64_t hash = 0;
        std::uint64_t uid_device = 0;
        std::uint64_t uid_file = 0;
        bool missing = false;
    };

    Workspace& workspace;
    const SessionStore& store;
    std::string workspace_root;

    llvm::SmallVector<TrackedSource> sources;

    /// Workspace sweep baseline.
    llvm::DenseMap<Fid, FileState> baseline;

    /// True while a sweep is in flight (it suspends between batches);
    /// concurrent ticks are skipped instead of racing on the baseline.
    bool sweeping = false;
};

}  // namespace clice
