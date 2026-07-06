#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "server/context/context_resolver.h"
#include "server/session/session_store.h"
#include "server/workspace/workspace.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

/// A change to a file the server cares about, described by what happened —
/// not by what must be invalidated. Events are plain values handed to
/// MasterServer::dispatch(); there is no log and no replay (this is not
/// event sourcing), the event batch is just the call's argument.
struct FileEvent {
    enum class Kind : std::uint8_t {
        /// didOpen installed a fresh buffer for the file.
        BufferOpened,
        /// didChange folded edits into the open buffer.
        BufferEdited,
        /// didSave: the on-disk content now matches the buffer.
        BufferSaved,
        /// didClose dropped the buffer; disk is the truth again.
        BufferClosed,
        /// The file's content changed on disk behind the server's back
        /// (emitted by the FileTracker's workspace poll).
        DiskChanged,
        /// The file disappeared from disk (see DiskChanged).
        DiskRemoved,
        /// The compilation database was reloaded; `cdb` lists the files
        /// whose entries were added, removed or changed.
        CDBChanged,
        /// clice/switchContext changed the file's active header context.
        ContextChanged,
        /// A stateful worker crashed; `paths` lists the documents it owned.
        WorkerCrashed,
    };

    /// CDBChanged payload: the reload's per-file delta, as master path-pool
    /// ids. `changed` means the file kept an entry but its command differs.
    struct CDBDelta {
        llvm::SmallVector<std::uint32_t, 0> added;
        llvm::SmallVector<std::uint32_t, 0> removed;
        llvm::SmallVector<std::uint32_t, 0> changed;

        bool empty() const {
            return added.empty() && removed.empty() && changed.empty();
        }
    };

    Kind kind;
    std::uint32_t path_id = no_path_id;
    /// WorkerCrashed only: the crashed worker's lost documents.
    llvm::SmallVector<std::uint32_t> paths;
    /// CDBChanged only: the reload delta.
    CDBDelta cdb;

    static FileEvent buffer_opened(std::uint32_t path_id) {
        return {Kind::BufferOpened, path_id};
    }

    static FileEvent buffer_edited(std::uint32_t path_id) {
        return {Kind::BufferEdited, path_id};
    }

    static FileEvent buffer_saved(std::uint32_t path_id) {
        return {Kind::BufferSaved, path_id};
    }

    static FileEvent buffer_closed(std::uint32_t path_id) {
        return {Kind::BufferClosed, path_id};
    }

    static FileEvent disk_changed(std::uint32_t path_id) {
        return {Kind::DiskChanged, path_id};
    }

    static FileEvent disk_removed(std::uint32_t path_id) {
        return {Kind::DiskRemoved, path_id};
    }

    static FileEvent cdb_changed(CDBDelta delta) {
        FileEvent event{Kind::CDBChanged};
        event.cdb = std::move(delta);
        return event;
    }

    static FileEvent context_changed(std::uint32_t path_id) {
        return {Kind::ContextChanged, path_id};
    }

    static FileEvent worker_crashed(llvm::ArrayRef<std::uint32_t> lost_documents) {
        FileEvent event{Kind::WorkerCrashed};
        event.paths.assign(lost_documents.begin(), lost_documents.end());
        return event;
    }
};

/// The effects an event batch demands, deduplicated. The engine computes
/// these; MasterServer::dispatch() executes them against the mutable
/// services (sessions, context resolver, background indexer).
struct DirtySet {
    /// Compile inputs changed: ast_dirty + trial_done=false + forget the
    /// cached self-containment verdict.
    llvm::SmallVector<std::uint32_t> mark_ast_dirty;
    /// Built AST lost (worker crash) but compile inputs did not change:
    /// recompile only, without re-running the header trial or touching the
    /// self-containment verdict.
    llvm::SmallVector<std::uint32_t> mark_lost;
    /// Re-run the header trial only (trial_done=false); the AST itself is
    /// not stale.
    llvm::SmallVector<std::uint32_t> reset_trial;
    /// The header's content (or its preamble chain) changed: drop its
    /// persisted self-containment verdict so the next compile re-earns it.
    /// Executed by the context resolver, which owns the verdicts.
    llvm::SmallVector<std::uint32_t> reset_header_mode;
    /// Header sessions whose synthesized preamble embeds changed content:
    /// zero deps.build_at so every chain file is re-validated by hash, plus
    /// the mark_ast_dirty treatment.
    llvm::SmallVector<std::uint32_t> force_revalidate;
    /// Closed files whose index entries went stale: enqueue for background
    /// reindexing.
    llvm::SmallVector<std::uint32_t> enqueue_reindex;
    /// Headers whose resolved context borrows a compile command that no
    /// longer exists in that form (the host's CDB entry changed): drop the
    /// context so the next use re-resolves. Content validation cannot see
    /// a flag change, so neither force_revalidate nor the deps snapshot
    /// covers this. Executed by the context resolver.
    llvm::SmallVector<std::uint32_t> drop_context;
    /// Include edges changed: context choices may now be orphaned; run the
    /// context resolver's orphan cleanup.
    bool recheck_contexts = false;
    /// Persist the workspace cache snapshot.
    bool save_cache = false;
    /// Kick the background indexer's scheduler.
    bool reschedule_indexing = false;
    /// A CDB reload may have introduced the first C++20 modules; create the
    /// compile graph if it does not exist yet. Executed by the dispatcher,
    /// which owns the Compiler.
    bool ensure_compile_graph = false;

    bool empty() const {
        return mark_ast_dirty.empty() && mark_lost.empty() && reset_trial.empty() &&
               reset_header_mode.empty() && force_revalidate.empty() && enqueue_reindex.empty() &&
               drop_context.empty() && !recheck_contexts && !save_cache && !reschedule_indexing &&
               !ensure_compile_graph;
    }
};

/// The invalidation engine: the single place that maps file events onto
/// derived-state invalidation.
///
/// Ownership charter:
///   - reads the session store and the context resolver, never mutates them;
///   - directly updates the derived graphs Workspace owns (include graph,
///     module map, ...);
///   - anything touching Sessions, context-domain state (verdicts, choices,
///     header contexts), the index queue, or cache persistence is returned
///     as a DirtySet effect and executed by the dispatcher, so the engine
///     stays testable with plain data structures.
///
/// The engine is told about every event, but the buffer-change mechanics of
/// BufferOpened/BufferEdited (text, version, ast_dirty, generation) are the
/// SessionStore's charter and stay in apply_open/apply_change; those cases
/// exist as hooks for future cross-file policy, not as the sync path.
class Invalidator {
public:
    /// Read a file's current on-disk content, or nullopt if unreadable.
    /// Defaults to the real filesystem; unit tests inject file content
    /// through it. Used by BufferSaved to detect a save hook or formatter
    /// rewriting the file as it lands (disk ahead of the buffer).
    using ReadFile = std::function<std::optional<std::string>(llvm::StringRef path)>;

    Invalidator(Workspace& workspace,
                const SessionStore& store,
                const ContextResolver& contexts,
                ReadFile read_file = {});

    /// Fold a batch of events into one deduplicated effect set.
    DirtySet apply(llvm::ArrayRef<FileEvent> events);

private:
    /// The invalidation cascade for "this file's on-disk content is new":
    /// rescan the file's disk state, then split every affected file into
    /// open (recompile) and closed (reindex). Shared by BufferSaved (disk
    /// now holds the buffer) and DiskChanged on closed files (disk changed
    /// behind the server's back), and used verbatim — the two differ only
    /// in what the caller adds around it.
    void cascade_disk_content_change(std::uint32_t path_id, DirtySet& dirty);

    /// Cascade a module unit's compile-graph invalidation (PCM caches,
    /// dependent module units), splitting dirtied units open/closed.
    void cascade_compile_graph(std::uint32_t path_id, DirtySet& dirty);

    Workspace& workspace;
    const SessionStore& store;
    const ContextResolver& contexts;
    ReadFile read_file;
};

}  // namespace clice
