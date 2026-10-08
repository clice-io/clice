module;

#include "modules/prelude.h"

module clice:server.invalidator;

import :project.index_store;
import :project.project;
import :server.ast_projection;
import :server.editor_context;
import :server.session_store;

namespace clice {

class PCMFamily;

/// A change to a file the server cares about, described by what happened —
/// not by what must be invalidated. Events are plain values handed to
/// MasterServer::dispatch(); there is no log and no replay (this is not
/// event sourcing), the event batch is just the call's argument.
struct FileEvent {
    enum class Kind : std::uint8_t {
        /// The file's content on disk changed — open or not: the file table
        /// saw other bytes than it last saw, whoever looked (a background
        /// tick, a didSave, a rescan, a compile's staleness check).
        DiskChanged,
        /// The file disappeared from disk (see DiskChanged).
        DiskRemoved,
        /// The build's commands changed: a compilation database was
        /// reloaded, a source joined a default-command rule, or a saved
        /// file was recorded as a provisional member (Build::record). `cdb`
        /// lists the files whose entries were added, removed or changed;
        /// the invalidator works out the provisional members' own changes.
        CDBChanged,
        /// A stateful worker crashed; `paths` lists the documents it owned.
        WorkerCrashed,
        /// A stateful worker evicted the document from its LRU cache: the
        /// built AST is gone, but unlike a crash only this one document is
        /// affected.
        DocumentEvicted,
    };

    Kind kind;
    Fid path_id;
    /// WorkerCrashed only: the crashed worker's lost documents.
    llvm::SmallVector<Fid> paths;
    /// CDBChanged only: the delta its producer saw.
    CDBDiff cdb;

    static FileEvent disk_changed(Fid path_id) {
        return {Kind::DiskChanged, path_id};
    }

    static FileEvent disk_removed(Fid path_id) {
        return {Kind::DiskRemoved, path_id};
    }

    static FileEvent cdb_changed(CDBDiff delta) {
        FileEvent event{Kind::CDBChanged};
        event.cdb = std::move(delta);
        return event;
    }

    static FileEvent worker_crashed(llvm::ArrayRef<Fid> lost_documents) {
        FileEvent event{Kind::WorkerCrashed};
        event.paths.assign(lost_documents.begin(), lost_documents.end());
        return event;
    }

    static FileEvent document_evicted(Fid path_id) {
        return {Kind::DocumentEvicted, path_id};
    }
};

/// The disk changes the file table saw since the last call, as events: a
/// file seen missing is DiskRemoved, any other change DiskChanged.
llvm::SmallVector<FileEvent> take_disk_events(FileTable& files);

/// The effects an event batch demands, deduplicated. The engine computes
/// these; MasterServer::dispatch() executes them against the mutable
/// services (sessions, editor context, background indexer).
///
/// Effect algebra: the sets are not disjoint, and stronger effects subsume
/// weaker ones on the same file — mark_ast_dirty implies forgetting the
/// self-containment impression, which reset_header_mode extends to a
/// persisted verdict — and one event may push a file into several sets.
/// Execution is idempotent per effect, so the overlap is harmless; what
/// matters is that each set can also occur ALONE (reset_header_mode for a
/// closed header has no AST to dirty), which is why they are separate
/// vocabulary rather than severity levels of one list.
struct DirtySet {
    /// Compile inputs changed: ast_dirty + trial_done=false + forget the
    /// cached self-containment verdict.
    llvm::SmallVector<Fid> mark_ast_dirty;
    /// Built AST lost (worker crash) but compile inputs did not change:
    /// recompile only, without re-running the header trial or touching the
    /// self-containment verdict.
    llvm::SmallVector<Fid> mark_lost;
    /// A header's content other than its open buffer, or its include chain,
    /// changed: drop its self-containment verdict so the next compile
    /// re-earns it.
    /// Executed by the command resolver, which owns the verdicts.
    llvm::SmallVector<Fid> reset_header_mode;
    /// Closed files whose own content changed: their index rows describe
    /// text that no longer exists. Enqueue for background reindexing as
    /// ReindexReason::ContentChanged — queries skip these files'
    /// contributions until the reindex lands.
    llvm::SmallVector<Fid> reindex_content_changed;
    /// Closed files enqueued only because a dependency changed: their own
    /// rows are positionally intact. Enqueue as ReindexReason::DepsOnly —
    /// queries keep serving the previous rows. A file in both lists is
    /// ContentChanged (the indexer's reason upgrade is absorbing).
    llvm::SmallVector<Fid> reindex_deps_only;

    /// Files whose pending-reindex state must be discarded: a removed file
    /// has nothing left to reindex, and a stale ContentChanged reason would
    /// otherwise suppress its (deliberately still-serving) shard forever.
    llvm::SmallVector<Fid> clear_reindex;

    /// The three reindex effect lists are kept disjoint per file, in event
    /// order: a batch can hold delete-then-recreate (atomic saves) as well
    /// as change-then-delete, so neither "clear wins" nor "enqueue wins" is
    /// right as a fixed rule — the later event for a given file wins. All
    /// emission goes through these adders to keep that true by construction,
    /// letting the executor apply the lists in any order.
    void add_reindex_content_changed(Fid path_id) {
        erase_id(clear_reindex, path_id);
        reindex_content_changed.push_back(path_id);
    }

    void add_reindex_deps_only(Fid path_id) {
        erase_id(clear_reindex, path_id);
        reindex_deps_only.push_back(path_id);
    }

    void add_clear_reindex(Fid path_id) {
        erase_id(reindex_content_changed, path_id);
        erase_id(reindex_deps_only, path_id);
        // A disk removal retains the last-known index, so it also cancels an
        // earlier entry-change drop — a surviving drop would mask the shard
        // and let the next save retire it. The reverse order needs no
        // handling: every drop emission is paired with a reindex adder,
        // which already un-clears.
        erase_id(drop_index, path_id);
        if(llvm::find(clear_reindex, path_id) == clear_reindex.end()) {
            clear_reindex.push_back(path_id);
        }
    }

    /// A TU the build stopped compiling — its database still loads but no
    /// longer lists it: the rows leave the index and nothing is owed, unlike
    /// a file that vanished from disk, whose shard stays behind (queries
    /// withhold it).
    void add_retire(Fid path_id) {
        erase_id(reindex_content_changed, path_id);
        erase_id(reindex_deps_only, path_id);
        if(llvm::find(clear_reindex, path_id) == clear_reindex.end()) {
            clear_reindex.push_back(path_id);
        }
        if(llvm::find(drop_index, path_id) == drop_index.end()) {
            drop_index.push_back(path_id);
        }
    }

private:
    static void erase_id(llvm::SmallVector<Fid>& ids, Fid path_id) {
        ids.erase(std::remove(ids.begin(), ids.end(), path_id), ids.end());
    }

public:
    /// TUs whose compile command changed: their index describes a compile
    /// that no longer exists, and content-based freshness cannot see that.
    /// The indexer drops the manifest, contributions and persisted blobs —
    /// a surviving manifest would judge the queued reindex fresh and keep
    /// the old-command rows serving, in this session and after a restart.
    /// Follows the later-event rule above: a later removal's clear cancels
    /// the drop, since the deleted file's index stays behind.
    llvm::SmallVector<Fid> drop_index;
    /// Headers whose resolved context was derived from something that
    /// changed — the host's CDB entry, a file along the include chain:
    /// drop the context so the next use re-resolves. Executed by the
    /// editor context.
    llvm::SmallVector<Fid> drop_context;
    /// Include edges changed: context choices may now be orphaned; run the
    /// editor context's orphan cleanup.
    bool recheck_contexts = false;
    /// Kick the background indexer's scheduler.
    bool reschedule_indexing = false;

    bool empty() const {
        return mark_ast_dirty.empty() && mark_lost.empty() && reset_header_mode.empty() &&
               reindex_content_changed.empty() && reindex_deps_only.empty() &&
               clear_reindex.empty() && drop_index.empty() && drop_context.empty() &&
               !recheck_contexts && !reschedule_indexing;
    }
};

/// The invalidation engine: the single place that maps file events onto
/// derived-state invalidation.
///
/// Ownership charter:
///   - reads the session store, the editor context and the index store's
///     recorded header hosts, never mutates them;
///   - directly updates the derived graphs Project owns (include graph,
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
/// ast_dirty, generation — a buffer shadows the disk for its own file's
/// compile only, so no other file reads it, and there is no event for
/// opening, editing or closing one), and so
/// does clice/switchContext's session reset (single owner, synchronous,
/// no cascade). Anything failing a clause goes through the pipeline — do
/// not add ceremonial event kinds for exempt logic.
class Invalidator {
public:
    Invalidator(Project& project,
                const SessionStore& store,
                const EditorContext& contexts,
                const ASTProjectionTable& projections,
                PCMFamily& pcm,
                const IndexStore& index);

    /// Fold a batch of events into one deduplicated effect set.
    DirtySet apply(llvm::ArrayRef<FileEvent> events);

    /// The root TUs and open documents whose compiles depend on the file:
    /// the ones the lexical scan sees including it, and the ones whose
    /// compiles read it, imported a module built from it, or looked for it
    /// — the scan cannot resolve a macro include, never follows an import,
    /// and never sees a file before it exists.
    llvm::SmallVector<Fid> readers(Fid path_id) const;

private:
    /// Rescan the file's disk state (include edges, module maps). A module
    /// name the rescan gave its first provider cascades to the consumers
    /// holding sentinel edges against it; a name the file stopped
    /// providing cascades through the provider's real node instead.
    void rescan_disk_state(Fid path_id, DirtySet& dirty);

    /// The invalidation cascade for "this file's on-disk content is new":
    /// rescan the file's disk state, then split every affected file into
    /// open (recompile) and closed (reindex).
    void cascade_disk_content_change(Fid path_id, DirtySet& dirty);

    /// Cascade a module unit's compile-graph invalidation (PCM caches,
    /// dependent module units), splitting dirtied units open/closed.
    void cascade_compile_graph(Fid path_id, DirtySet& dirty);

    /// A module name just gained its first provider: cascade through its
    /// sentinel node and route the dirtied consumers to recompiles and
    /// ContentChanged reindexes.
    void provider_appeared(llvm::StringRef module_name, DirtySet& dirty);

    /// See the definition: the open/closed/index-only split of a
    /// dependency invalidation.
    void mark_dependent(Fid path_id, DirtySet& dirty);

    Project& project;
    const SessionStore& store;
    const EditorContext& contexts;
    const ASTProjectionTable& projections;
    PCMFamily& pcm;
    const IndexStore& index;
};

}  // namespace clice
