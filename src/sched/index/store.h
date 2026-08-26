#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "sched/workspace.h"

#include "kota/async/async.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace testing {

struct IndexerFixture;

}

/// The project index's storage engine: merging TUIndex results into the
/// in-memory ProjectIndex and Shard blobs, persisting them, and restoring
/// them at startup. It knows nothing about sessions or the pump's debt
/// ledger — every row-changing entry point returns a neutral Report and the
/// callers route it: the pump claims the reindex debt, the serving adapter
/// decides which row changes need a client refresh.
class IndexStore {
public:
    /// What one row-changing entry point did to state others own.
    /// Collections are deduplicated.
    class Report {
    public:
        /// TUs owed a ContentChanged reindex: their rows are missing,
        /// stale or discarded and no in-process event would rebuild them.
        /// The pump claims these before the current attempt settles.
        llvm::ArrayRef<std::uint32_t> reindex() const {
            return reindex_ids;
        }

        /// Files whose stored rows were replaced, re-masked or dropped
        /// while possibly index-served: the serving adapter checks which
        /// of them an open session actually serves and refreshes those.
        llvm::ArrayRef<std::uint32_t> rows_changed() const {
            return rows_ids;
        }

        /// Debt surfaced after this save() serialized its CDB snapshot:
        /// the persisted standalone-debt record is stale. A shutdown owes
        /// one metadata retry; a live session's next save covers it.
        bool snapshot_stale = false;

        void add_reindex(std::uint32_t id) {
            if(reindex_seen.insert(id).second) {
                reindex_ids.push_back(id);
            }
        }

        void add_rows_changed(std::uint32_t id) {
            if(rows_seen.insert(id).second) {
                rows_ids.push_back(id);
            }
        }

        bool empty() const {
            return reindex_ids.empty() && rows_ids.empty() && !snapshot_stale;
        }

    private:
        llvm::SmallVector<std::uint32_t> reindex_ids;
        llvm::SmallVector<std::uint32_t> rows_ids;
        llvm::DenseSet<std::uint32_t> reindex_seen;
        llvm::DenseSet<std::uint32_t> rows_seen;
    };

    struct LoadResult {
        /// False when a global blob existed but could not be decoded (old
        /// format or corrupt): the server rebuilds from scratch, but a
        /// read-only reader must report an unusable cache instead of an
        /// empty index.
        bool decoded = true;
        Report report;
    };

    IndexStore(kota::event_loop& loop, Workspace& workspace);

    /// Merge a TUIndex result: intern FileVersions, replace the TU's
    /// manifest, and write row blobs only for variants no shard stores yet
    /// — a re-index whose rows are unchanged records its contributions and
    /// touches nothing else. Returns nullopt when the result failed
    /// verification and nothing was committed — the caller must count the
    /// file as failed, not indexed.
    std::optional<Report> merge(const void* tu_index_data, std::size_t size);

    /// Drop a TU's index wholesale: manifest and contributions now (the
    /// affected shards' live masks follow), persisted blobs at the next
    /// save. For invalidation content-based freshness cannot see — a
    /// compile-command change — where a surviving manifest would keep
    /// judging the old-command rows fresh, in this session and after a
    /// restart.
    Report drop_index(std::uint32_t tu_path_id);

    /// Persist the dirty state (rewritten shards, replaced manifests, the
    /// global blob) through the index storage. Serialization runs on the
    /// event loop from copies; the write batch is offloaded to the kota
    /// thread pool. Shards whose variant set shrank are compacted first.
    ///
    /// `debt` is the pump's immutable debt snapshot at call time: the CDB
    /// snapshot persists its standalone entries so a dropped header's
    /// repair debt survives a process exit. Owner debt the compaction
    /// discovers before serializing joins the same snapshot; debt surfaced
    /// after it (write-time corruption recovery) comes back in the report
    /// with snapshot_stale set.
    kota::task<Report> save(llvm::SmallVector<std::uint32_t> debt);

    /// Load the global blob, adopt every resolvable manifest, fetch the
    /// shard blobs the contributions expect, and sweep the rest.
    /// `read_only` keeps the sweeps in memory only: an out-of-process
    /// reader (`clice index --stats`) must not delete blobs a concurrently
    /// running server may be about to reference.
    LoadResult load(bool read_only = false);

    /// Record the host source whose command a standalone-indexed header's
    /// retained rows borrowed. Written when a merge lands, persisted in
    /// the CDB snapshot for the offline invalidation diff.
    void record_header_host(std::uint32_t header, std::uint32_t host) {
        header_hosts[header] = host;
    }

    /// Start a freshness round: FileVersion verdicts hold for one round —
    /// the disk can change under a running round, but staleness is
    /// re-judged per round anyway.
    void begin_round() {
        fv_verdicts.clear();
    }

    /// Check whether a file needs re-indexing: no manifest, or a stale
    /// FileVersion among its dependencies. Valid only within one round:
    /// the verdicts are cleared by begin_round(), never here.
    bool need_update(llvm::StringRef file_path);

    /// Shard blobs whose write has not durably completed: dirty since the
    /// last save plus the batch a running save is committing. The gauge
    /// reaches zero only once every shard write settled — never in the
    /// window where save() has snapshot-cleared the dirty set but its
    /// commit (and the last_save_shards update) is still in flight.
    std::size_t pending_shard_writes() const {
        return dirty_shards.size() + saving_shards;
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

private:
    friend struct testing::IndexerFixture;

    kota::event_loop& loop;
    Workspace& workspace;

    /// Blobs mutated since the last save, plus whether the global blob
    /// (symbols, FileVersion table) changed.
    llvm::DenseSet<std::uint32_t> dirty_shards;
    llvm::DenseSet<std::uint32_t> dirty_manifests;
    bool global_dirty = false;

    /// Blob removals discovered during load (stale manifests, orphan
    /// shards, swept layouts), deferred into the first save so startup
    /// never runs synchronous database commits on the event loop.
    llvm::SmallVector<index::BlobKey> startup_removes;

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

    /// Filter the debt candidates down to standalone TUs the CDB snapshot
    /// must record: no manifest pin and no CDB entry means the snapshot is
    /// the only record that an index is owed.
    llvm::SmallVector<std::uint32_t> standalone_of(llvm::ArrayRef<std::uint32_t> candidates);

    /// drop_index body, appending into the caller's report — reconcile
    /// drops several TUs into the one load report.
    void drop_index_into(std::uint32_t tu_path_id, Report& report);

    /// Diff the persisted CDB snapshot against the live CDB and drop the
    /// index of every TU whose compile command changed while no server was
    /// running — content-based freshness cannot see command changes, so an
    /// adopted manifest would keep judging the old-command rows fresh, in
    /// this session and after a restart. Entries that vanished keep their
    /// index (last-known content still serves navigation), mirroring the
    /// live CDB-reload treatment.
    void reconcile_cdb_snapshot(Report& report);

    /// Per-round FileVersion staleness verdicts: many TUs share the same
    /// versions, and one stat (or repair) per version per round is enough.
    llvm::DenseMap<std::uint32_t, bool> fv_verdicts;

    /// Two-layer staleness test on a FileVersion, cached per round; a hash
    /// match after a stat mismatch repairs the version's stat fast path in
    /// place for every consumer.
    bool file_version_stale(std::uint32_t fv_id);

    /// Add every TU contributing to `path_id`'s shard to the report's
    /// reindex debt. Used when the file's resident rows are lost while its
    /// manifests still read fresh: no in-process event would ever rebuild
    /// them, and for standalone headers no restart sweep would either.
    void requeue_owners(std::uint32_t path_id, Report& report);

    /// Drop every resident shard that may borrow database memory —
    /// everything not dirty, since dirty shards own their bytes by
    /// construction (merges install memory copies) — and requeue the
    /// owners of the dropped rows.
    void shed_borrowed_shards(Report& report);

    /// Runtime-corruption recovery, shared by the write-time and the
    /// snapshot-migration detection points: nothing in the condemned
    /// database survives, so borrowed shards are shed with their owners
    /// requeued while every manifest, the global and the CDB snapshot
    /// re-dirty to re-persist into the freshly opened database.
    void recover_corrupt_database(Report& report);

    /// Confirmed corruption heals through rebuildability: condemn the
    /// database (deleted on close) and continue on a freshly opened empty
    /// one, so the session's rebuild persists instead of waiting for the
    /// next start. A failed reopen (another process grabbed the writer
    /// lock meanwhile) leaves persistence disabled for the session.
    void reopen_fresh_database();

    /// Migrate resident shards onto a fresh database read snapshot after a
    /// save's commit (growing the map first when the write hit a full one),
    /// then retire the previous snapshot. Filesystem-backed runs return
    /// immediately: their buffers are immortal.
    kota::task<> migrate_shard_views(Report& report);

    std::size_t saved_shards = 0;

    /// Shards in the batch a running save() is committing (see
    /// pending_shard_writes).
    std::size_t saving_shards = 0;
};

}  // namespace clice
