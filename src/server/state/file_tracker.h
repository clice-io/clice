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

    /// One CDB poll tick. When no rule declares a source, registers every
    /// database discovery finds that is not watched yet, at the root and
    /// its direct subdirectories and above every open file still without
    /// a command. Stats every registered source — declared ones that do
    /// not exist yet included, which is how a database generated after
    /// startup is picked up — and the response files its commands name
    /// (the first `watched_responses` every tick, the rest every
    /// `response_tail_period` ticks, so a generator emitting one per unit
    /// costs a bounded number of stats). Once a source's stamp
    /// change has stayed stable for two consecutive ticks, reloads it and
    /// emits one CDBChanged event carrying the reload's diff. A discovered
    /// database vanishing or returning flips its presence, and the files
    /// whose default entry moves with it change command (see
    /// Build::source_order); its entries keep serving meanwhile.
    ///
    /// `force` reloads unconditionally: it skips both the stamp gate and
    /// the two-tick settling debounce (the half-written-file guard). The
    /// test hook uses it so a single poll request applies a change
    /// deterministically; a spurious forced reload just yields an empty
    /// diff.
    llvm::SmallVector<FileEvent> tick_cdb(bool force = false);

    /// Register, load and watch the databases in the directories from the
    /// file's up to the workspace root, which startup discovery (the root
    /// and its direct subdirectories) did not look at: a file of a deeper
    /// project compiles from its own database. Nothing when a rule declares
    /// sources, the file has a command already, or it lies outside the
    /// workspace. The loads' diffs, as CDBChanged events.
    llvm::SmallVector<FileEvent> discover_around(Fid path_id);

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
    /// (existence, size, mtime, filesystem identity) of a file: a
    /// rename-over with a forged equal size and mtime still changes the
    /// UniqueID.
    struct FileStamp {
        bool exists = false;
        std::uint64_t size = 0;
        std::int64_t mtime_ns = 0;
        std::uint64_t uid_device = 0;
        std::uint64_t uid_file = 0;

        friend bool operator==(const FileStamp&, const FileStamp&) = default;
    };

    static FileStamp stat_file(llvm::StringRef path);

    /// The stamp of a source: its database and the response files its
    /// commands name.
    struct SourceStamp {
        FileStamp database;
        llvm::SmallVector<FileStamp> responses;

        friend bool operator==(const SourceStamp&, const SourceStamp&) = default;
    };

    /// `carry` supplies the stamps of the response files beyond
    /// `watched_responses` on the ticks that skip them; null stats all.
    SourceStamp stat_source(SourceID id, const SourceStamp* carry) const;

    /// One registered source's watch state.
    struct TrackedSource {
        SourceID id;
        /// The stamp the loaded entries correspond to.
        SourceStamp applied;
        /// Debounce: the stamp observed on the previous tick, not yet settled.
        SourceStamp pending;
        bool has_pending = false;
        /// The last load (the startup one included) first named response
        /// files, whose stamps could only be taken after it read them:
        /// reload once more, so a rewrite landing in between is not
        /// missed.
        bool reread = false;
    };

    /// Register `id` for watching, baselined at its current stamp.
    void track(SourceID id);

    /// Seed the sweep baseline of `path_id` at its current content, unless
    /// it has one; a file a database load just added to the graph would
    /// otherwise be seeded at whatever the first sweep finds, an edit in
    /// between silently taken as the original.
    void seed(Fid path_id);

    /// Tick one source; the reload's events, if any.
    void tick_source(TrackedSource& tracked, bool force, CDBDiff& delta);

    /// discover_around's work, its delta merged into `delta`.
    void discover_into(Fid path_id, CDBDiff& delta);

    /// The files the source and another one both list: the ones whose
    /// default entry may move with the source's presence.
    llvm::SmallVector<Fid> shared_files(SourceID id) const;

    /// The source of each file's default entry, as the build ranks them
    /// now; none for a file the build no longer compiles.
    llvm::SmallVector<std::optional<SourceID>> default_sources(llvm::ArrayRef<Fid> files) const;

    constexpr static std::size_t watched_responses = 64;
    constexpr static std::uint64_t response_tail_period = 10;
    std::uint64_t cdb_ticks = 0;

    /// Last-known on-disk state of a tracked file.
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
