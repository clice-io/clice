#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "server/compiler/context_resolver.h"
#include "server/state/session_store.h"
#include "server/state/workspace.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
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
        /// A stateful worker crashed; `paths` lists the documents it owned.
        WorkerCrashed,
        /// A stateful worker evicted the document from its LRU cache: the
        /// built AST is gone, but unlike a crash only this one document is
        /// affected.
        DocumentEvicted,
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

    static FileEvent worker_crashed(llvm::ArrayRef<std::uint32_t> lost_documents) {
        FileEvent event{Kind::WorkerCrashed};
        event.paths.assign(lost_documents.begin(), lost_documents.end());
        return event;
    }

    static FileEvent document_evicted(std::uint32_t path_id) {
        return {Kind::DocumentEvicted, path_id};
    }
};

/// The effects an event batch demands, deduplicated. The engine computes
/// these; MasterServer::dispatch() executes them against the mutable
/// services (sessions, context resolver, background indexer).
///
/// Effect algebra: the sets are not disjoint, and stronger effects subsume
/// weaker ones on the same file — mark_ast_dirty implies the trial reset
/// that reset_trial asks for, force_revalidate implies mark_ast_dirty's
/// session treatment, and one event may push a file into several sets
/// (BufferSaved emits both reset_trial and reset_header_mode for the saved
/// file). Execution is idempotent per effect, so the overlap is harmless;
/// what matters is that each set can also occur ALONE (reset_trial without
/// mark_ast_dirty re-runs the trial on a clean AST), which is why they are
/// separate vocabulary rather than severity levels of one list.
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
    /// drop the chain snapshot's fast paths so every chain file is
    /// re-validated by hash, plus the mark_ast_dirty treatment.
    llvm::SmallVector<std::uint32_t> force_revalidate;
    /// Closed files whose own content changed: their index rows describe
    /// text that no longer exists. Enqueue for background reindexing as
    /// ReindexReason::ContentChanged — queries skip these files'
    /// contributions until the reindex lands.
    llvm::SmallVector<std::uint32_t> reindex_content_changed;
    /// Closed files enqueued only because a dependency changed: their own
    /// rows are positionally intact. Enqueue as ReindexReason::DepsOnly —
    /// queries keep serving the previous rows. A file in both lists is
    /// ContentChanged (the indexer's reason upgrade is absorbing).
    llvm::SmallVector<std::uint32_t> reindex_deps_only;

    /// Files whose pending-reindex state must be discarded: a removed file
    /// has nothing left to reindex, and a stale ContentChanged reason would
    /// otherwise suppress its (deliberately still-serving) shard forever.
    llvm::SmallVector<std::uint32_t> clear_reindex;

    /// The three reindex effect lists are kept disjoint per file, in event
    /// order: a batch can hold delete-then-recreate (atomic saves) as well
    /// as change-then-delete, so neither "clear wins" nor "enqueue wins" is
    /// right as a fixed rule — the later event for a given file wins. All
    /// emission goes through these adders to keep that true by construction,
    /// letting the executor apply the lists in any order.
    void add_reindex_content_changed(std::uint32_t path_id) {
        erase_id(clear_reindex, path_id);
        reindex_content_changed.push_back(path_id);
    }

    void add_reindex_deps_only(std::uint32_t path_id) {
        erase_id(clear_reindex, path_id);
        reindex_deps_only.push_back(path_id);
    }

    void add_clear_reindex(std::uint32_t path_id) {
        erase_id(reindex_content_changed, path_id);
        erase_id(reindex_deps_only, path_id);
        if(llvm::find(clear_reindex, path_id) == clear_reindex.end()) {
            clear_reindex.push_back(path_id);
        }
    }

private:
    static void erase_id(llvm::SmallVector<std::uint32_t>& ids, std::uint32_t path_id) {
        ids.erase(std::remove(ids.begin(), ids.end(), path_id), ids.end());
    }

public:
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
               reset_header_mode.empty() && force_revalidate.empty() &&
               reindex_content_changed.empty() && reindex_deps_only.empty() &&
               clear_reindex.empty() && drop_context.empty() && !recheck_contexts && !save_cache &&
               !reschedule_indexing && !ensure_compile_graph;
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
/// Exemption criterion — invalidation logic may bypass apply() if and only
/// if it (1) has no cross-file cascade, (2) touches only a single owner's
/// state, and (3) completes within one synchronous section. SessionStore's
/// buffer mechanics qualify (apply_open/apply_change own text, version,
/// ast_dirty, generation; the BufferOpened/BufferEdited cases below exist
/// as hooks for future cross-file policy, not as the sync path), and so
/// does clice/switchContext's session reset (single owner, synchronous,
/// no cascade). Anything failing a clause goes through the pipeline — do
/// not add ceremonial event kinds for exempt logic.
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
