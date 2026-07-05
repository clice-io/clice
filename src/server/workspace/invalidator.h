#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

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
        /// The file changed on disk behind the server's back. No producer
        /// yet — reserved for the disk poller / cache validation side.
        DiskChanged,
        /// The file disappeared from disk. No producer yet (see DiskChanged).
        DiskRemoved,
        /// clice/switchContext changed the file's active header context.
        ContextChanged,
        /// A stateful worker crashed; `paths` lists the documents it owned.
        WorkerCrashed,
    };

    Kind kind;
    std::uint32_t path_id = no_path_id;
    /// WorkerCrashed only: the crashed worker's lost documents.
    llvm::SmallVector<std::uint32_t> paths;

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
    /// Header sessions whose synthesized preamble embeds changed content:
    /// zero deps.build_at so every chain file is re-validated by hash, plus
    /// the mark_ast_dirty treatment.
    llvm::SmallVector<std::uint32_t> force_revalidate;
    /// Closed files whose index entries went stale: enqueue for background
    /// reindexing.
    llvm::SmallVector<std::uint32_t> enqueue_reindex;
    /// Include edges changed: context choices may now be orphaned; run the
    /// context resolver's orphan cleanup.
    bool recheck_contexts = false;
    /// Persist the workspace cache snapshot.
    bool save_cache = false;
    /// Kick the background indexer's scheduler.
    bool reschedule_indexing = false;

    bool empty() const {
        return mark_ast_dirty.empty() && mark_lost.empty() && reset_trial.empty() &&
               force_revalidate.empty() && enqueue_reindex.empty() && !recheck_contexts &&
               !save_cache && !reschedule_indexing;
    }
};

/// The invalidation engine: the single place that maps file events onto
/// derived-state invalidation.
///
/// Ownership charter:
///   - reads the session store, never mutates it;
///   - directly updates the derived graphs Workspace owns (include graph,
///     module map, header modes, ...);
///   - anything touching Sessions, the index queue, or cache persistence is
///     returned as a DirtySet effect and executed by the dispatcher, so the
///     engine stays testable with just a Workspace and a SessionStore.
///
/// The engine is told about every event, but the buffer-change mechanics of
/// BufferOpened/BufferEdited (text, version, ast_dirty, generation) are the
/// SessionStore's charter and stay in apply_open/apply_change; those cases
/// exist as hooks for future cross-file policy, not as the sync path.
class Invalidator {
public:
    /// Read a file's current on-disk content, or nullopt if unreadable.
    /// Defaults to the real filesystem; unit tests inject file content
    /// through it. Reserved for the Disk* events' content validation.
    using ReadFile = std::function<std::optional<std::string>(llvm::StringRef path)>;

    Invalidator(Workspace& workspace, const SessionStore& store, ReadFile read_file = {});

    /// Fold a batch of events into one deduplicated effect set.
    DirtySet apply(llvm::ArrayRef<FileEvent> events);

private:
    Workspace& workspace;
    const SessionStore& store;
    ReadFile read_file;
};

}  // namespace clice
