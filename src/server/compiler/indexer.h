#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "server/state/workspace.h"
#include "support/signal.h"

#include "kota/async/async.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class ContextResolver;
struct SessionStore;
class WorkerPool;

namespace testing {

struct IndexerFixture;

}

/// Why a file awaits re-indexing. The invalidation engine knows the cause
/// at enqueue time, so queries can decide in O(1) whether a pending file's
/// existing index rows are still trustworthy (see IndexQuery's freshness
/// contract).
enum class ReindexReason : std::uint8_t {
    /// Enqueued by a dependency cascade (or a bulk sweep of unknown
    /// staleness): the file's own content is not known to have changed, so
    /// its index rows are positionally intact — at worst semantically
    /// behind — and keep serving until the reindex lands.
    DepsOnly,
    /// The file's own content changed: its index rows describe text that
    /// no longer exists, so queries skip this file's contribution until
    /// the reindex lands.
    ContentChanged,
};

/// Background indexing scheduler.
///
/// Indexer owns the indexing queue and drives disk files through
/// the stateless workers, merging each TUIndex result into Workspace's
/// ProjectIndex (manifests, FileVersions, symbols) and Shard blobs.  It
/// holds no index data of its own beyond the dirty bookkeeping.
///
/// Responsibilities:
///   - Background indexing scheduling (enqueue → idle timer → worker dispatch)
///   - Merging TUIndex results into Workspace's index state
///   - Persisting and restoring the index blobs
///
/// NOT responsible for:
///   - Index queries — handled by IndexQuery
///   - Compilation — handled by Compiler
///   - Document lifecycle — handled by MasterServer
class Indexer {
public:
    Indexer(kota::event_loop& loop,
            Workspace& workspace,
            WorkerPool& pool,
            ContextResolver& contexts,
            const SessionStore& sessions) :
        loop(loop), bg_tasks(loop), workspace(workspace), pool(pool), contexts(contexts),
        sessions(sessions) {}

    /// Whether open files' disk snapshots are indexed like closed ones.
    /// Off by default: the LSP side never reads an open file's shard (its
    /// session serves it), so the work would be pure waste — until an
    /// agent shows up, whose disk-truth queries need those shards. Turned
    /// on (sticky) by the first agentic index query; files closed before
    /// that are already covered, because BufferClosed re-enqueues a file
    /// whose shard does not match the disk.
    bool index_open_files = false;

    /// Temporarily pause background indexing to give priority to user
    /// requests.  Indexing tasks already dispatched to workers continue,
    /// but no new tasks will be sent until resume_indexing() is called.
    void pause_indexing();

    /// Resume background indexing after a pause.
    void resume_indexing();

    /// RAII guard that pauses indexing for its lifetime.
    struct [[nodiscard]] ScopedPause {
        Indexer& indexer;

        explicit ScopedPause(Indexer& idx) : indexer(idx) {
            indexer.pause_indexing();
        }

        ~ScopedPause() {
            indexer.resume_indexing();
        }

        ScopedPause(const ScopedPause&) = delete;
        ScopedPause& operator=(const ScopedPause&) = delete;
    };

    ScopedPause scoped_pause() {
        return ScopedPause{*this};
    }

    /// Add a file to the background indexing queue. A file enqueued twice
    /// keeps a single queue entry; its reason is upgraded to ContentChanged
    /// if either enqueue says so (a file both cascaded onto and edited is
    /// as stale as the edit makes it).
    void enqueue(std::uint32_t server_path_id, ReindexReason reason);

    /// Why the file awaits re-indexing (queued or currently being indexed),
    /// or nullopt when its index is not pending an update. O(1), no I/O —
    /// the query path calls this per candidate file.
    std::optional<ReindexReason> pending_reason(std::uint32_t server_path_id) const {
        auto it = reindex_reasons.find(server_path_id);
        if(it == reindex_reasons.end()) {
            return std::nullopt;
        }
        return it->second.reason;
    }

    /// Forget a file's pending-reindex state (reason and queue membership):
    /// used when the file is removed from disk — nothing is left to reindex,
    /// and a lingering ContentChanged reason would suppress its deliberately
    /// still-serving shard forever. A queue slot already consumed stays
    /// consumed; one not yet consumed is skipped at dispatch time (the
    /// consume loop treats a missing pending entry as a cleared slot).
    void clear_pending(std::uint32_t server_path_id) {
        reindex_reasons.erase(server_path_id);
        pending_ids.erase(server_path_id);
    }

    /// Schedule background indexing (respects idle timeout and dedup).
    void schedule();

    /// Merge a TUIndex result: intern FileVersions, replace the TU's
    /// manifest, and write row blobs only for variants no shard stores yet
    /// — a re-index whose rows are unchanged records its contributions and
    /// touches nothing else. Returns false when the result failed
    /// verification and nothing was committed — the caller must count the
    /// file as failed, not indexed.
    bool merge(const void* tu_index_data, std::size_t size);

    /// Drop a TU's index wholesale: manifest and contributions now (the
    /// affected shards' live masks follow), persisted blobs at the next
    /// save. For invalidation content-based freshness cannot see — a
    /// compile-command change — where a surviving manifest would keep
    /// judging the old-command rows fresh, in this session and after a
    /// restart.
    void drop_index(std::uint32_t tu_path_id);

    /// Persist the dirty state (rewritten shards, replaced manifests, the
    /// global blob) through the index storage. Serialization runs on the
    /// event loop from copies; the write batch is offloaded to the kota
    /// thread pool. Shards whose variant set shrank are compacted first.
    kota::task<> save();

    /// Load the global blob, adopt every resolvable manifest, fetch the
    /// shard blobs the contributions expect, and sweep the rest.
    /// `read_only` keeps the sweeps in memory only: an out-of-process
    /// reader (`clice index --stats`) must not delete blobs a concurrently
    /// running server may be about to reference. Returns false when a
    /// global blob existed but could not be decoded (old format or
    /// corrupt): the server rebuilds from scratch, but a read-only reader
    /// must report an unusable cache instead of an empty index.
    bool load(bool read_only = false);

    /// Shard blobs whose write has not durably completed: dirty since the
    /// last save plus the batch a running save is committing. The gauge
    /// reaches zero only once every shard write settled — never in the
    /// window where save() has snapshot-cleared the dirty set but its
    /// commit (and the last_save_shards update) is still in flight.
    std::size_t pending_shard_writes() const {
        return dirty_shards.size() + saving_shards;
    }

    /// Cancel background indexing and wait for all tasks to settle.
    kota::task<> stop();

    /// Whether background indexing is currently idle (no active or queued work).
    bool is_idle() const {
        return !indexing_active && index_queue_pos >= index_queue.size();
    }

    /// Number of files remaining in the indexing queue.
    std::size_t pending_files() const {
        return index_queue_pos < index_queue.size() ? index_queue.size() - index_queue_pos : 0;
    }

    /// Total files that were enqueued in the current (or last) indexing round.
    std::size_t total_queued() const {
        return index_queue.size();
    }

    /// Files whose latest index attempt failed for good — rejected by the
    /// worker, an empty or unverifiable result, a spent crash budget, or a
    /// dead IPC path — with no retry pending. Their rows are missing or
    /// stale; a later successful pass removes them again. The one-shot
    /// `clice index` reports a partial build from this.
    std::size_t failed_files() const {
        return failed_ids.size();
    }

    /// How many shard blobs the last save() durably committed. A
    /// steady-state save commits 0 — only variant-set changes rewrite a
    /// blob — so the stats endpoint can pin full-rewrite regressions.
    std::size_t last_save_shards() const {
        return saved_shards;
    }

    /// Whether index state remains that no save() committed. After a final
    /// save this means write failures whose retry never came — the one-shot
    /// `clice index` must not report a durable index from this.
    bool has_unsaved_state() const {
        return !dirty_shards.empty() || !dirty_manifests.empty() || global_dirty || cdb_dirty;
    }

    /// Progress of the current (or last) indexing round. The reporter reads
    /// this on each on_progress_changed emission — the signal only wakes it,
    /// the numbers live here.
    struct Progress {
        enum class Stage : std::uint8_t { Begin, Report, End };
        Stage stage = Stage::Begin;
        std::size_t total = 0;
        std::size_t completed = 0;
        std::size_t dispatched = 0;
    };

    const Progress& progress() const {
        return progress_data;
    }

    /// Emitted whenever the indexing progress state changes (round begins, a
    /// file completes, round ends). A subscriber reads progress() on wake.
    Signal<> on_progress_changed;

private:
    kota::event_loop& loop;
    kota::task_group<> bg_tasks;
    Workspace& workspace;
    WorkerPool& pool;
    ContextResolver& contexts;
    const SessionStore& sessions;

    /// Open documents, read-only. A file with an open Session is skipped by
    /// background indexing (its buffer index is authoritative).

    /// Background indexing queue and scheduling state.  pending_ids mirrors
    /// the un-consumed tail of index_queue so enqueue can dedupe; the queue
    /// is compacted once a round has fully drained.
    std::vector<std::uint32_t> index_queue;
    llvm::DenseSet<std::uint32_t> pending_ids;
    std::size_t index_queue_pos = 0;

    /// The pending-reindex state machine, per file. This block is the
    /// authoritative description; every rule below exists because its
    /// absence was a concrete bug.
    ///
    /// States: absent → queued (slot in index_queue + entry here) →
    /// in-flight (slot consumed, entry alive) → absent again.
    ///
    /// Invariants:
    /// 1. index_one is the ONLY place that decides to skip work (open
    ///    session, or hash-fresh shard for deps-only slots). Duplicating
    ///    those checks elsewhere reintroduces reason-blind skips.
    /// 2. need_update() may shortcut deps-only slots ONLY: the engine
    ///    observed content changes itself, and the dep-hash check cannot
    ///    see a file's own edit.
    /// 3. The merge lands iff the entry is alive and no ContentChanged
    ///    enqueue happened after launch (content_ticket <= launch ticket).
    ///    Deps-only requeues do not discard an in-flight pass.
    /// 4. Completion erases the entry iff ticket == launch ticket: a
    ///    requeue during the flight must survive the older task's clear.
    /// 5. Within a queued slot, ContentChanged absorbs; a fresh slot after
    ///    consumption carries its own reason (the consumed pass owns the
    ///    earlier debt).
    /// 6. clear_pending (file removal) drops entry and queue membership;
    ///    the orphaned slot is skipped at dispatch.
    /// 7. Queries suppress a file's contributions iff its entry's reason
    ///    is ContentChanged (see pending_reason).
    struct PendingReindex {
        ReindexReason reason;
        std::uint64_t ticket;
        /// Ticket of the newest ContentChanged enqueue. The merge guard
        /// compares against this, not `ticket`: a deps-only requeue during a
        /// flight bumps `ticket` (to survive the completion clear) but must
        /// not discard an in-flight content pass — its rows are positionally
        /// right and the follow-up slot redoes the semantic drift anyway.
        std::uint64_t content_ticket;

        /// Crash requeues of this entry. Bounds the damage of a poison
        /// file that reliably crashes workers: without a cap, every
        /// requeue would burn another worker's crash budget until the
        /// whole pool is dead. Preemption requeues do not count — they
        /// say nothing about the file.
        unsigned requeue_attempts = 0;
    };

    /// What a failed index dispatch did to the file's pending state.
    enum class RequeueVerdict : std::uint8_t {
        /// No pending entry survived (file removed mid-flight).
        Dropped,
        /// The failed dispatch carried bytes older than the pending
        /// content; the newer content's own queued slot redoes the work.
        Superseded,
        /// The crash budget is spent; the file is abandoned.
        GaveUp,
        /// Re-enqueued for the next round.
        Requeued,
    };

    constexpr static unsigned max_requeue_attempts = 3;

    /// Requeue a file whose index dispatch failed with a crash or a
    /// memory-pressure preemption. `ticket` is the failed dispatch's launch
    /// ticket: a crash of superseded bytes says nothing about the current
    /// content and must not spend its budget. Only crashes spend the
    /// bounded budget: a preemption says nothing about the file, and
    /// capping it would silently drop coverage — the pending reason is
    /// erased after the attempt, so the stale shard would be served as
    /// fresh.
    RequeueVerdict note_dispatch_failure(std::uint32_t server_path_id,
                                         std::uint64_t ticket,
                                         bool crashed);

    friend struct testing::IndexerFixture;

    /// Blobs mutated since the last save, plus whether the global blob
    /// (symbols, FileVersion table) changed.
    llvm::DenseSet<std::uint32_t> dirty_shards;
    llvm::DenseSet<std::uint32_t> dirty_manifests;
    bool global_dirty = false;

    /// The persisted CDB snapshot blob's bytes as last read or written;
    /// empty when none exists. save() rewrites the blob whenever the live
    /// CDB serializes differently.
    std::string persisted_cdb_snapshot;

    /// The persisted CDB snapshot needs a rewrite no dirty blob will
    /// trigger: its write failed while the rest of the batch may have
    /// landed, or load() found it missing or corrupt next to a valid
    /// global. Without this flag the rewrite would wait for an unrelated
    /// dirtying merge: a save with nothing else to commit skips the
    /// snapshot recompute entirely.
    bool cdb_dirty = false;

    /// Host source whose command each standalone-indexed header's retained
    /// rows borrowed, recorded when a merge lands and persisted in the CDB
    /// snapshot.
    /// The offline invalidator checks the recorded host directly — the
    /// include graph is rebuilt from the NEW commands before load(), so
    /// reachability alone cannot see a change that removed or redirected
    /// the very include edge the header's context came through.
    llvm::DenseMap<std::uint32_t, std::uint32_t> header_hosts;

    /// Standalone TUs owed an index that nothing else records: no manifest
    /// (dropped for a command or rule change, the rebuild failed or is
    /// still pending) and no CDB entry the startup sweep would retry. The
    /// snapshot keeps an entry for each so reconcile's debt pass retries
    /// them next session instead of the index staying silently partial.
    llvm::SmallVector<std::uint32_t> standalone_debt();

    /// Diff the persisted CDB snapshot against the live CDB and drop the
    /// index of every TU whose compile command changed while no server was
    /// running — content-based freshness cannot see command changes, so an
    /// adopted manifest would keep serving the old-command rows forever.
    /// Entries that vanished keep their index (last-known content still
    /// serves navigation), mirroring the live CDB-reload treatment.
    void reconcile_cdb_snapshot();

    /// Per-round FileVersion staleness verdicts: many TUs share the same
    /// versions, and one stat (or repair) per version per round is enough.
    /// Cleared when a round starts.
    llvm::DenseMap<std::uint32_t, bool> fv_verdicts;

    /// Two-layer staleness test on a FileVersion, cached per round; a hash
    /// match after a stat mismatch repairs the version's stat fast path in
    /// place for every consumer.
    bool file_version_stale(std::uint32_t fv_id);

    /// Check whether a file needs re-indexing: no manifest, or a stale
    /// FileVersion among its dependencies. Valid only within one round:
    /// the verdicts above are cleared when a round starts, never here.
    bool need_update(llvm::StringRef file_path);

    llvm::DenseMap<std::uint32_t, PendingReindex> reindex_reasons;
    llvm::DenseSet<std::uint32_t> failed_ids;
    std::uint64_t reindex_ticket = 0;
    bool indexing_active = false;
    bool indexing_scheduled = false;
    std::size_t saved_shards = 0;
    /// Shards in the batch a running save() is committing (see
    /// pending_shard_writes).
    std::size_t saving_shards = 0;
    std::shared_ptr<kota::timer> index_idle_timer;

    /// Pause/resume: when paused, new index tasks wait on this event.
    /// Uses a counter so nested pause/resume pairs work correctly.
    std::size_t pause_depth = 0;
    kota::event resume_event{true};

    Progress progress_data;

    kota::task<> run_background_indexing();
    kota::task<> index_one(std::uint32_t server_path_id,
                           std::uint64_t ticket,
                           std::size_t index,
                           std::size_t total);

    /// One dispatched unit of a background round: index the file, then end
    /// its pending window (ticket-guarded) and report progress. `completed`
    /// refers into run_background_indexing's frame, which outlives every
    /// spawned task (it joins them before returning).
    kota::task<> run_index_task(std::uint32_t server_path_id,
                                std::uint64_t ticket,
                                std::size_t index,
                                std::size_t total,
                                std::size_t& completed);
};

}  // namespace clice
