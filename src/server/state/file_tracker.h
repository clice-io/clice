#pragma once

#include <cstdint>
#include <string>

#include "server/state/invalidator.h"
#include "server/state/session_store.h"
#include "server/state/workspace.h"

#include "kota/async/async.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

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
    /// (existence, size, mtime) identity of the CDB file.
    struct CDBStamp {
        bool exists = false;
        std::uint64_t size = 0;
        std::int64_t mtime_ns = 0;

        friend bool operator==(const CDBStamp&, const CDBStamp&) = default;
    };

    /// Stat a CDB candidate. The workspace loader calls this BEFORE
    /// reading the file and hands the stamp to the constructor: the
    /// baseline must describe the content the load consumed. Statting
    /// after the load leaves a window where a rewrite lands in between
    /// and is never reloaded — the stamp then gates against the new file
    /// while memory holds the old commands, permanently. The reversed
    /// race (a rewrite between this stat and the read) only costs one
    /// spurious reload with an empty diff.
    static CDBStamp stat_file(llvm::StringRef path);

    /// Discovers the compile_commands.json itself (it may not exist yet).
    /// `consumed_path`/`consumed` describe the database the workspace
    /// load actually read (see stat_file); when discovery lands on the
    /// same path, that stamp seeds `applied`. Construct after the
    /// workspace is loaded.
    FileTracker(Workspace& workspace,
                const SessionStore& store,
                std::string workspace_root,
                llvm::StringRef consumed_path,
                CDBStamp consumed);

    /// Refresh a file's baseline from the disk right now, silently, and
    /// report whether its content differs from the recorded baseline.
    /// Called on didSave: the save's cascade already handles the change,
    /// so the next sweep must not re-report it — and a save that did not
    /// change the bytes reports false, letting the dispatcher skip the
    /// cascade entirely. Unknown or unreadable files report true (the
    /// conservative direction: cascade rather than miss).
    bool reseed(std::uint32_t path_id);

    /// One CDB poll tick. Stats the known compile_commands.json — or keeps
    /// discovering one when none was found yet, which is how a database
    /// generated after startup is picked up. Once a (size, mtime) change
    /// has stayed stable for two consecutive ticks, reloads the CDB and
    /// emits one CDBChanged event carrying the reload's diff.
    ///
    /// `force` reloads unconditionally: it skips both the (size, mtime)
    /// stamp gate — which could hide a same-size rewrite landing within
    /// mtime granularity — and the two-tick settling debounce (the
    /// half-written-file guard). The test hook uses it so a single poll
    /// request applies a change deterministically; a spurious forced
    /// reload just yields an empty diff.
    llvm::SmallVector<FileEvent> tick_cdb(bool force = false);

    /// One workspace sweep. Stats every file the dependency graph knows —
    /// open buffers included: their disk state matters to dependents (who
    /// always read the disk), so an external write to an open file emits
    /// DiskChanged like any other. didSave keeps the baseline in step via
    /// reseed(). A (mtime, size) suspect is confirmed by content hash
    /// before DiskChanged is emitted, so touch-only changes (mtime bump,
    /// identical bytes) stay silent. A stat failure on a known file emits
    /// DiskRemoved once; a transient content-read failure emits nothing
    /// and is retried on the next tick.
    ///
    /// Files seen for the first time seed the baseline and normally emit
    /// nothing — the first sweep after startup is silent by construction
    /// (startup storm guard). One exception: a file whose index shard
    /// exists but no longer matches the disk changed before the seed
    /// could see it, and that change still needs dispatching.
    ///
    /// Stats run synchronously in batches, yielding to the event loop
    /// between batches; each round's duration is perf-logged.
    /// TODO: offload stats to the thread pool (and consider a directory
    /// listing cache for Windows, where per-file stat is expensive) if the
    /// logged sweep timing shows the need.
    kota::task<llvm::SmallVector<FileEvent>> tick_workspace();

private:
    CDBStamp stat_cdb() const;

    /// Last-known on-disk state of a tracked file.
    struct FileState {
        std::uint64_t size = 0;
        std::int64_t mtime_ns = 0;
        std::uint64_t hash = 0;
        bool missing = false;
    };

    Workspace& workspace;
    const SessionStore& store;
    std::string workspace_root;

    /// compile_commands.json path; empty until one is discovered.
    std::string cdb_path;
    /// The stamp the currently loaded CDB entries correspond to.
    CDBStamp applied;
    /// Debounce: the stamp observed on the previous tick, not yet settled.
    CDBStamp pending;
    bool has_pending = false;

    /// Workspace sweep baseline.
    llvm::DenseMap<std::uint32_t, FileState> baseline;

    /// True while a sweep is in flight (it suspends between batches);
    /// concurrent ticks are skipped instead of racing on the baseline.
    bool sweeping = false;
};

}  // namespace clice
