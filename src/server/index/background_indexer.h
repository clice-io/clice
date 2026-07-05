#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "server/workspace/workspace.h"
#include "support/signal.h"

#include "kota/async/async.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class ContextResolver;
class WorkerPool;
struct SessionStore;

/// Background indexing scheduler.
///
/// BackgroundIndexer owns the indexing queue and drives disk files through
/// the stateless workers, merging each TUIndex result into Workspace's
/// ProjectIndex and MergedIndex shards.  It holds no index data of its own.
///
/// Responsibilities:
///   - Background indexing scheduling (enqueue → idle timer → worker dispatch)
///   - Merging TUIndex results into Workspace's ProjectIndex
///   - Persisting and restoring the index shards
///
/// NOT responsible for:
///   - Index queries — handled by IndexQuery
///   - Compilation — handled by Compiler
///   - Document lifecycle — handled by MasterServer
class BackgroundIndexer {
public:
    BackgroundIndexer(kota::event_loop& loop,
                      Workspace& workspace,
                      WorkerPool& pool,
                      ContextResolver& contexts,
                      const SessionStore& sessions) :
        loop(loop), bg_tasks(loop), workspace(workspace), pool(pool), contexts(contexts),
        sessions(sessions) {}

    /// Temporarily pause background indexing to give priority to user
    /// requests.  Indexing tasks already dispatched to workers continue,
    /// but no new tasks will be sent until resume_indexing() is called.
    void pause_indexing();

    /// Resume background indexing after a pause.
    void resume_indexing();

    /// RAII guard that pauses indexing for its lifetime.
    struct [[nodiscard]] ScopedPause {
        BackgroundIndexer& indexer;

        explicit ScopedPause(BackgroundIndexer& idx) : indexer(idx) {
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

    /// Add a file to the background indexing queue.
    void enqueue(std::uint32_t server_path_id);

    /// Schedule background indexing (respects idle timeout and dedup).
    void schedule();

    /// Merge a TUIndex result into Workspace's ProjectIndex and MergedIndex shards.
    void merge(const void* tu_index_data, std::size_t size);

    /// Save Workspace's ProjectIndex and MergedIndex shards to the cache
    /// store ("index" namespace, Persistent policy).  Serialization runs
    /// on the event loop; each blob's commit (fsync + rename) is offloaded
    /// to the kota thread pool.
    kota::task<> save();

    /// Load Workspace's ProjectIndex and MergedIndex shards from the cache
    /// store, sweeping orphaned shard blobs.
    void load();

    /// Check whether a file needs re-indexing (stale or missing shard).
    bool need_update(llvm::StringRef file_path);

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

    /// Open documents, read-only. A file with an open Session is skipped by
    /// background indexing (its buffer index is authoritative).
    const SessionStore& sessions;

    /// Background indexing queue and scheduling state.  pending_ids mirrors
    /// the un-consumed tail of index_queue so enqueue can dedupe; the queue
    /// is compacted once a round has fully drained.
    std::vector<std::uint32_t> index_queue;
    llvm::DenseSet<std::uint32_t> pending_ids;
    std::size_t index_queue_pos = 0;
    bool indexing_active = false;
    bool indexing_scheduled = false;
    std::shared_ptr<kota::timer> index_idle_timer;

    /// Pause/resume: when paused, new index tasks wait on this event.
    /// Uses a counter so nested pause/resume pairs work correctly.
    std::size_t pause_depth = 0;
    kota::event resume_event{true};

    Progress progress_data;

    kota::task<> run_background_indexing();
    kota::task<> index_one(std::uint32_t server_path_id, std::size_t index, std::size_t total);
};

}  // namespace clice
