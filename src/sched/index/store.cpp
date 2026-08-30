#include "sched/index/store.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <utility>
#include <vector>

#include "index/database.h"
#include "index/manifest.h"
#include "index/shard.h"
#include "index/tu_index.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"

#include "kota/codec/json/json.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice {

namespace {

/// Stable blob key for a file's shard or a TU's manifest: runtime pool ids
/// are per-session, so blobs are named by a hash of the path instead.
std::string blob_key(llvm::StringRef path) {
    return std::format("{:016x}", llvm::xxh3_64bits(path));
}

/// JSON layout of the persisted CDB snapshot (blob kind CDB): per source
/// file, the sorted canonical command hashes of its entries and a hash of
/// its matched config rules when the index state was last saved. A
/// standalone-indexed header gets an entry too (empty hashes): its own
/// matched rules plus the host source whose command its rows borrowed.
struct CDBSnapshotEntry {
    std::string file;
    std::vector<std::string> hashes;

    /// Entry hash of the file's default selection (the candidate-order
    /// winner). The hash multiset alone cannot see an offline flip of the
    /// winner — candidates unchanged, selection changed.
    std::string selected;

    std::string rules;
    std::string host;
};

struct CDBSnapshot {
    std::vector<CDBSnapshotEntry> entries;
};

/// clice.toml append/remove rules change the effective indexing command
/// without touching the CDB entry, so the snapshot must cover them too —
/// an offline rule edit is as stale-making as an offline command edit.
std::string rules_hash(const Config& config, llvm::StringRef file) {
    std::vector<std::string> append, remove;
    config.match_rules(file, append, remove);
    if(append.empty() && remove.empty()) {
        return {};
    }
    std::string joined;
    for(auto& arg: append) {
        joined += 'a';
        joined += arg;
        joined += '\0';
    }
    for(auto& arg: remove) {
        joined += 'r';
        joined += arg;
        joined += '\0';
    }
    return std::format("{:016x}", llvm::xxh3_64bits(joined));
}

CDBSnapshot build_cdb_snapshot(Workspace& workspace,
                               const llvm::DenseMap<std::uint32_t, std::uint32_t>& header_hosts,
                               llvm::ArrayRef<std::uint32_t> standalone_debt) {
    CDBSnapshot snapshot;
    for(auto& [path_id, hashes]: workspace.cdb.command_hash_snapshot()) {
        auto file = workspace.path_pool.resolve(path_id).str();
        auto rules = rules_hash(workspace.config, file);
        snapshot.entries.push_back({
            .file = std::move(file),
            .hashes = {hashes.begin(), hashes.end()},
            .selected = workspace.cdb.selected_hash(path_id).value_or(std::string()),
            .rules = std::move(rules),
        });
    }
    // Standalone-indexed TUs have no CDB entry, yet their effective command
    // depends on their own matched rules and their borrowed host's command
    // — both must be snapshot to detect offline changes.
    auto add_standalone = [&](std::uint32_t tu) {
        auto file = workspace.path_pool.resolve(tu);
        if(workspace.cdb.has_entry(file)) {
            return;
        }
        auto host_it = header_hosts.find(tu);
        snapshot.entries.push_back({
            .file = file.str(),
            .rules = rules_hash(workspace.config, file),
            .host = host_it != header_hosts.end()
                        ? workspace.path_pool.resolve(host_it->second).str()
                        : std::string(),
        });
    };
    for(auto tu: llvm::make_first_range(workspace.project_index.manifests)) {
        add_standalone(tu);
    }
    // A dropped standalone TU whose rebuild has not landed keeps its entry:
    // with no manifest pin and no CDB entry, the snapshot is the only
    // record that an index is owed (reconcile's debt pass retries it).
    for(auto tu: standalone_debt) {
        add_standalone(tu);
    }
    // Deterministic bytes: save() decides "unchanged" by byte equality.
    std::ranges::sort(snapshot.entries, {}, &CDBSnapshotEntry::file);
    return snapshot;
}

std::string serialize_cdb_snapshot(Workspace& workspace,
                                   const llvm::DenseMap<std::uint32_t, std::uint32_t>& header_hosts,
                                   llvm::ArrayRef<std::uint32_t> standalone_debt) {
    auto json =
        kota::codec::json::to_string(build_cdb_snapshot(workspace, header_hosts, standalone_debt));
    return json ? std::move(*json) : std::string();
}

}  // namespace

IndexStore::IndexStore(kota::event_loop& loop, Workspace& workspace) :
    loop(loop), workspace(workspace) {}

std::optional<IndexStore::Report> IndexStore::merge(const void* tu_index_data, std::size_t size) {
    // Zero-copy consumption: the wire stays serialized; a new variant's
    // blob bytes are sliced out and installed or merged without decoding
    // the envelope, and only genuinely new symbol names are materialized.
    auto view =
        index::TUIndex::from_bytes(llvm::StringRef(static_cast<const char*>(tu_index_data), size));
    if(!view.loaded()) {
        LOG_WARN("Ignoring TUIndex that failed verification");
        return std::nullopt;
    }
    auto main_local_id = view.path_count() - 1;
    llvm::StringRef main_tu_path = view.path(main_local_id);

    // Interning paths only names them — pool ids left behind by a rejected
    // result are inert. Everything that is index STATE (symbols,
    // FileVersions, the manifest, shards) commits only below the section
    // loop, once every part of the result validated.
    auto& project = workspace.project_index;
    llvm::SmallVector<std::uint32_t> file_ids_map;
    file_ids_map.resize_for_overwrite(view.path_count());
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        file_ids_map[i] = workspace.path_pool.intern(view.path(i));
    }
    auto tu_path_id = file_ids_map[main_local_id];

    index::TUManifest manifest;
    manifest.built_at = static_cast<std::uint64_t>(view.built_at());

    std::size_t hits = 0;
    std::size_t appended = 0;
    // Staged, not committed: a section that fails verification rejects the
    // whole result mid-loop, and shards installed before that point would
    // leave the surviving manifest referencing variants the new blobs no
    // longer store.
    llvm::SmallVector<std::pair<std::uint32_t, index::Shard>> replacements;
    // (TU-local path id, variant identity) per serving section; the
    // FileVersions these will reference are interned only at commit.
    llvm::SmallVector<std::pair<std::uint32_t, std::uint64_t>> section_contributions;
    llvm::SmallVector<std::uint32_t> rebuilt_ids;
    // TU-local path id -> content hash of the bytes each section's rows
    // were built from (0 = no section). A section's shard already records
    // that hash, so the FileVersion baseline below adopts it: pairing the
    // rows with any other hash — the file behind a PCM whose disk moved
    // on under a preserved or backdated mtime — would keep the baseline
    // fresh while queries serve another generation's rows.
    llvm::SmallVector<std::uint64_t> consumed_hashes(view.path_count(), 0);
    auto record_consumed = [&](std::uint32_t local_id, std::uint64_t content_hash) {
        auto path_hash = view.path_hash(local_id);
        if(path_hash != 0 && path_hash != content_hash) {
            LOG_WARN("Reject merge for {}: rows for {} consumed other content than the compiler",
                     main_tu_path,
                     workspace.path_pool.resolve(file_ids_map[local_id]));
            return false;
        }
        consumed_hashes[local_id] = content_hash;
        return true;
    };
    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        auto local_id = view.section_path(section);
        auto blob_hash = view.section_hash(section);
        auto global_id = file_ids_map[local_id];

        auto shard_it = workspace.shards.find(global_id);
        auto* shard = shard_it != workspace.shards.end() ? &shard_it->second : nullptr;

        // Fast path: the blob already stores this variant. The identity
        // hashes the blob bytes, which embed the content generation, so
        // one membership test is the whole check — no IO, no bytes read.
        if(shard && shard->loaded() && shard->has_variant(blob_hash)) {
            if(!record_consumed(local_id, shard->content_hash())) {
                return std::nullopt;
            }
            section_contributions.emplace_back(local_id, blob_hash);
            hits += 1;
            continue;
        }

        // The recomputed hash guards the variant identity alongside the
        // structural verification below: bytes installed under a hash they
        // do not reproduce would satisfy every later hit-path check for
        // that hash while the shard stores different rows. A mismatch (or
        // an invalid blob) leaves every recorded version matching the
        // disk, so an installed manifest would be judged fresh forever
        // with this file's rows missing or stale — reject the whole
        // result; nothing is committed yet.
        auto bytes = view.section_blob(section);
        if(llvm::xxh3_64bits(bytes) != blob_hash) {
            LOG_WARN("Reject merge for {}: rows section for {} failed verification",
                     main_tu_path,
                     workspace.path_pool.resolve(global_id));
            return std::nullopt;
        }
        auto fresh = index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
        if(!fresh.loaded()) {
            LOG_WARN("Reject merge for {}: rows for {} do not form a valid shard",
                     main_tu_path,
                     workspace.path_pool.resolve(global_id));
            return std::nullopt;
        }
        if(!record_consumed(local_id, fresh.content_hash())) {
            return std::nullopt;
        }

        index::Shard replacement;
        if(shard && shard->loaded() && shard->content_hash() == fresh.content_hash()) {
            // Same generation, new variant: merge it in, keeping every
            // stored variant — dead ones stay masked until the next save
            // compacts them.
            std::string merged;
            llvm::raw_string_ostream os(merged);
            index::merge_shards(*shard, shard->variants(), llvm::ArrayRef(fresh), os);
            replacement = index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(merged));
            assert(replacement.loaded() && "a freshly merged shard blob must verify");
            appended += 1;
        } else {
            // New content generation (or no blob at all): rows from other
            // generations must never share offset storage with these, so
            // the worker's bytes become the blob verbatim. Stale
            // contributions from other TUs stop matching any stored
            // variant; the commit re-enqueues their owners.
            replacement = std::move(fresh);
            rebuilt_ids.push_back(global_id);
        }
        replacements.emplace_back(global_id, std::move(replacement));
        section_contributions.emplace_back(local_id, blob_hash);
    }

    // The last gate and the first commit. A malformed reference bitmap (or
    // an out-of-range reference id) rejects the whole result for the same
    // reason a rows section that fails decode does above: everything the
    // merge would install reads as fresh forever, with the lost bits never
    // rebuilt.
    if(!project.merge(view, file_ids_map)) {
        LOG_WARN("Reject merge for {}: symbol reference bitmap failed verification", main_tu_path);
        return std::nullopt;
    }

    // Intern a FileVersion per file of the parse. The freshness baseline is
    // two-part and lives on the version, shared by every TU that consumed
    // it: the consumed-content hash from the compiler's own buffers, and a
    // stat fast path recorded only when the disk provably still holds the
    // consumed bytes — otherwise the stat could describe content the rows
    // were never built from, so those files re-earn their fast path
    // through a hash check instead (see file_version_stale).
    auto baseline_before_ns = fs::stat_baseline_before_ns(view.built_at());
    llvm::SmallVector<std::uint32_t> fv_of;
    fv_of.resize_for_overwrite(view.path_count());
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        llvm::StringRef path = view.path(i);
        // The section's own record wins: for a hashless path (behind a
        // PCM) it is the only hash naming the bytes the rows describe.
        auto hash = consumed_hashes[i] != 0 ? consumed_hashes[i] : view.path_hash(i);

        fs::file_status status;
        bool stat_ok = !fs::status(path, status);
        bool untouched = stat_ok && fs::mtime_ns(status) <= baseline_before_ns;
        if(hash == 0 && untouched) {
            // The worker had no buffer to hash (e.g. behind a PCM) and no
            // rows recorded one; the unchanged mtime proves the disk still
            // holds the consumed bytes, so hash it here.
            hash = hash_file(path);
        }

        auto fv = project.intern_file_version(file_ids_map[i], hash);
        if(untouched && hash != 0) {
            // The untouched mtime alone is no proof: a rewrite during the
            // build that preserves the size and backdates the mtime
            // (rsync -t) would stamp a stat describing bytes the rows were
            // never built from, and file_version_stale's equality fast
            // path would then judge them fresh forever. Stamp only under a
            // disk-hash match; an already-stamped version earned its stamp
            // the same way, so it need not re-prove it every merge.
            auto& record = project.file_versions.find(fv)->second;
            if(record.mtime_ns == 0 && hash_file(path) == hash) {
                record.size = status.getSize();
                record.mtime_ns = fs::mtime_ns(status);
            }
        }
        fv_of[i] = fv;
    }

    manifest.tu_fv = fv_of[main_local_id];
    manifest.nodes.reserve(view.location_count());
    for(std::uint32_t i = 0; i < view.location_count(); i += 1) {
        auto location = view.location(i);
        manifest.nodes.push_back({fv_of[location.path_id], location.include, location.line});
    }
    for(auto [local_id, rows_hash]: section_contributions) {
        manifest.contributions.emplace_back(fv_of[local_id], rows_hash);
    }

    Report report;
    for(auto& [global_id, replacement]: replacements) {
        workspace.shards[global_id] = std::move(replacement);
        dirty_shards.insert(global_id);
        report.add_rows_changed(global_id);
    }

    // Replace this TU's manifest wholesale: files it no longer touches lose
    // their contribution here, which is also what retires their variants —
    // no sweep over other shards is needed.
    auto affected = project.apply_manifest(tu_path_id, std::move(manifest));
    for(auto path_id: affected) {
        report.add_rows_changed(path_id);
        auto it = workspace.shards.find(path_id);
        if(it == workspace.shards.end()) {
            continue;
        }
        it->second.set_live(project.live_variants(path_id));
    }

    // A rebuild started its file's blob over, discarding the variants other
    // TUs' contributions pin. Those owners are usually already pending from
    // the same content event (enqueue dedupes); one whose attempt already
    // failed — or that reads fresh by hash after a revert, which is why
    // ContentChanged — has no in-process event left to rebuild its rows,
    // only a restart reaching load()'s re-enqueue.
    for(auto path_id: rebuilt_ids) {
        auto& shard = workspace.shards.find(path_id)->second;
        for(auto& [tu, hash]: project.contributions.find(path_id)->second) {
            if(!shard.has_variant(hash)) {
                report.add_reindex(tu);
            }
        }
    }
    dirty_manifests.insert(tu_path_id);
    global_dirty = true;

    LOG_INFO(
        "Merged TUIndex: {} paths, {} sections ({} hits, {} appended, {} rebuilt), "
        "{} merged_shards",
        view.path_count(),
        view.section_count(),
        hits,
        appended,
        rebuilt_ids.size(),
        workspace.shards.size());

    return report;
}

IndexStore::Report IndexStore::drop_index(std::uint32_t tu_path_id) {
    Report report;
    drop_index_into(tu_path_id, report);
    return report;
}

void IndexStore::drop_index_into(std::uint32_t tu_path_id, Report& report) {
    auto& project = workspace.project_index;
    if(!project.manifests.contains(tu_path_id)) {
        return;
    }
    // Dropped rows change index-served answers exactly like merged rows
    // do; without the refresh the client keeps them forever, since no
    // later merge or compile is owed.
    for(auto path_id: project.remove_manifest(tu_path_id)) {
        auto it = workspace.shards.find(path_id);
        if(it != workspace.shards.end()) {
            it->second.set_live(project.live_variants(path_id));
        }
        report.add_rows_changed(path_id);
    }
    dirty_manifests.insert(tu_path_id);
    global_dirty = true;
}

kota::task<IndexStore::Report> IndexStore::save(llvm::SmallVector<std::uint32_t> debt) {
    Report report;
    // Reset up front: every early return below means this save committed
    // nothing, and the gauge must not keep exposing the previous round's
    // count as current.
    saved_shards = 0;
    if(!workspace.index_db)
        co_return report;
    auto& db = *workspace.index_db;
    auto& project = workspace.project_index;
    ScopedTimer timer;

    // Compact shards whose variant set shrank: queries already mask the
    // dead rows, this erases them for real before the blob reaches disk.
    // A file with no live variant left in its blob is retired entirely
    // below: either no contribution remains, or the remaining ones pin
    // hashes a newer content generation replaced (their TUs have not
    // reindexed yet) — compacting to those pins would write a blob with no
    // variants at all, while retiring serves the same nothing the mask
    // already does. The pinning TUs are re-enqueued: no in-process event
    // would rebuild their rows otherwise (a reverted file even reads fresh
    // by hash), only a restart reaching load()'s re-enqueue.
    llvm::SmallVector<std::uint32_t> retired;
    for(auto& [path_id, shard]: workspace.shards) {
        auto live = project.live_variants(path_id);
        if(llvm::none_of(live, [&](std::uint64_t hash) { return shard.has_variant(hash); })) {
            retired.push_back(path_id);
            continue;
        }
        if(!shard.has_dead_variants()) {
            continue;
        }
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::merge_shards(shard, live, {}, os);
        auto replacement = index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
        assert(replacement.loaded() && "a freshly written shard blob must verify");
        shard = std::move(replacement);
        dirty_shards.insert(path_id);
    }
    for(auto path_id: retired) {
        workspace.shards.erase(path_id);
        dirty_shards.erase(path_id);
        report.add_rows_changed(path_id);
        requeue_owners(path_id, report);
    }

    // Snapshot the dirty state on the loop: everything below serializes
    // from copies, so merges landing across the write await simply re-dirty
    // for the next save. The id vectors parallel the batch so a failed
    // entry can be re-dirtied by its batch index.
    std::vector<index::BlobDatabase::Blob> batch;
    llvm::SmallVector<std::uint32_t> shard_ids;
    llvm::SmallVector<std::uint32_t> manifest_ids;
    llvm::SmallVector<index::BlobKey> removals = std::move(startup_removes);
    startup_removes.clear();
    for(auto path_id: retired) {
        removals.push_back(
            {index::IndexBlobKind::Shard, blob_key(workspace.path_pool.resolve(path_id))});
    }
    for(auto path_id: dirty_shards) {
        auto it = workspace.shards.find(path_id);
        assert(it != workspace.shards.end() && "dirty shards stay resident until retirement");
        batch.push_back({index::IndexBlobKind::Shard,
                         blob_key(workspace.path_pool.resolve(path_id)),
                         it->second.bytes().str()});
        shard_ids.push_back(path_id);
    }
    auto shard_count = batch.size();

    // One generation per persisted global blob, stamped into every
    // manifest of the batch and pinned per TU inside the global blob
    // (serialize_global): load() adopts a manifest only at its pinned
    // stamp. Without the pin an ordering check alone cannot tell a
    // deliberately unchanged older manifest from one whose update failed
    // while the global landed — both FileVersion sets can stay fully
    // resolvable (a reindex that changed rows or the include tree only).
    if(global_dirty) {
        project.global_generation += 1;
    }
    for(auto tu_path_id: dirty_manifests) {
        auto it = project.manifests.find(tu_path_id);
        auto key = blob_key(workspace.path_pool.resolve(tu_path_id));
        // Dirty with no in-memory manifest means dropped (drop_index): the
        // persisted blob must go too, or a restart resurrects the TU's
        // rows as fresh.
        if(it == project.manifests.end()) {
            removals.push_back({index::IndexBlobKind::Manifest, std::move(key)});
            continue;
        }
        it->second.global_gen = project.global_generation;
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        index::serialize_manifest(it->second, os);
        batch.push_back({index::IndexBlobKind::Manifest, std::move(key), std::move(bytes)});
        manifest_ids.push_back(tu_path_id);
    }
    auto manifest_count = batch.size() - shard_count;

    // The global blob follows the shards and manifests: a crash mid-batch
    // then strands only manifests, which load() drops by their generation
    // stamp — the reverse order would strand a global claiming symbols in
    // files whose rows never landed.
    if(global_dirty) {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        project.serialize_global(os, workspace.path_pool);
        batch.push_back({index::IndexBlobKind::Global, "global", std::move(bytes)});
    }

    // The CDB snapshot goes last, after the whole index state it
    // describes: landed before the global, a crash between the two would
    // leave the old global still pinning old-command manifests under a
    // snapshot that already matches the live CDB — reconcile would then
    // never drop them. A save that commits nothing skips the recompute
    // (unless the snapshot itself is owed a rewrite): a pure CDB change
    // dirties no blob on its own — the invalidator's drops do — so the
    // next dirtying save carries the fresh snapshot.
    // The debt input is the pump's snapshot at call time plus the owner
    // debt the compaction above discovered; anything surfacing past this
    // point is too late for these bytes and returns in the report.
    std::string cdb_bytes;
    std::optional<std::size_t> cdb_index;
    if(!batch.empty() || !removals.empty() || cdb_dirty) {
        debt.append(report.reindex().begin(), report.reindex().end());
        cdb_bytes = serialize_cdb_snapshot(workspace, header_hosts, standalone_of(debt));
        if(!cdb_bytes.empty() && cdb_bytes != persisted_cdb_snapshot) {
            cdb_index = batch.size();
            batch.push_back({index::IndexBlobKind::CDB, "cdb", cdb_bytes});
        } else {
            cdb_dirty = false;
        }
    }

    bool had_global = global_dirty;
    dirty_shards.clear();
    dirty_manifests.clear();
    global_dirty = false;

    // A deferred load-time sweep can name a key this very save re-writes:
    // the swept TU was re-enqueued at load and has already re-indexed.
    // Puts run before removes inside write(), so the stale removal would
    // delete the fresh blob — the put wins.
    if(!removals.empty()) {
        llvm::DenseSet<std::pair<unsigned, llvm::StringRef>> putting;
        for(auto& put: batch) {
            putting.insert({static_cast<unsigned>(put.kind), llvm::StringRef(put.key)});
        }
        llvm::erase_if(removals, [&](const index::BlobKey& remove) {
            return putting.contains(
                {static_cast<unsigned>(remove.kind), llvm::StringRef(remove.key)});
        });
    }

    if(batch.empty() && removals.empty()) {
        co_return report;
    }

    // The dirty set was snapshot-cleared above so merges landing across
    // the write await re-dirty for the next save; the in-flight count
    // keeps pending_shard_writes() truthful meanwhile — a stats reader
    // polling for "shard writes settled" must not observe zero while the
    // commit is still running (and saved_shards still holds its reset).
    saving_shards = shard_count;
    llvm::SmallVector<std::size_t> failed;
    // Cancellation while the write is still queued dequeues it (see
    // kota::queue): the batch dies with this frame, so the snapshot-
    // cleared dirty state must be restored or the final shutdown save
    // sees nothing to commit and reports saved progress that is absent
    // after restart. Restoration unions with whatever re-dirtied across
    // the await; the sweep keys in `removals` go back to the deferred
    // list they came from.
    auto restore = llvm::make_scope_exit([&] {
        dirty_shards.insert(shard_ids.begin(), shard_ids.end());
        dirty_manifests.insert(manifest_ids.begin(), manifest_ids.end());
        global_dirty = global_dirty || had_global;
        cdb_dirty = cdb_dirty || cdb_index.has_value();
        startup_removes.append(std::make_move_iterator(removals.begin()),
                               std::make_move_iterator(removals.end()));
        saving_shards = 0;
    });
    co_await kota::queue([&] { failed = db.write(batch, removals); });
    restore.release();
    // An entry the storage failed to commit is re-dirtied so a later save
    // retries it; discarded, the cache would trail the in-memory index
    // until an unrelated merge happens to dirty the same entry or a
    // restart rebuilds it. (Failed removals need no retry: load() drops
    // stale manifests by their generation pin and sweeps orphan shards.)
    std::size_t failed_shards = 0;
    for(auto i: failed) {
        if(i < shard_count) {
            failed_shards += 1;
            dirty_shards.insert(shard_ids[i]);
        } else if(i - shard_count < manifest_count) {
            dirty_manifests.insert(manifest_ids[i - shard_count]);
        } else if(cdb_index && i == *cdb_index) {
            // Keep the old persisted bytes and stay dirty so the next
            // save retries even when nothing else changes by then.
            cdb_dirty = true;
            cdb_index.reset();
        } else {
            global_dirty = true;
        }
    }
    if(cdb_index) {
        persisted_cdb_snapshot = std::move(cdb_bytes);
        cdb_dirty = false;
    }
    saved_shards = shard_count - failed_shards;
    saving_shards = 0;

    // Corruption can surface first at write time (a damaged page only the
    // write's tree descent reaches): heal like load-time corruption instead
    // of writing into the damaged environment every save. The batch's
    // shards — dirty at batch build, hence memory-backed — re-dirty before
    // the recovery's shed so they survive it and re-persist wholesale.
    if(db.corrupted()) {
        for(auto path_id: shard_ids) {
            dirty_shards.insert(path_id);
        }
        recover_corrupt_database(report);
        co_return report;
    }

    co_await migrate_shard_views(report);

    LOG_PERF("index",
             "phase=save shards={} manifests={} total={} elapsed_ms={}",
             shard_count,
             manifest_count,
             workspace.shards.size(),
             timer.ms());
    co_return report;
}

kota::task<> IndexStore::migrate_shard_views(Report& report) {
    if(!workspace.index_db) {
        co_return;
    }
    auto& db = *workspace.index_db;

    // A full-map write left nothing committed (everything is dirty again);
    // growing retires every snapshot at once, so the rebind below must run
    // to completion before the first yield.
    auto grown = db.grow();
    if(!grown) {
        // Degenerate (address space exhausted): borrowed views may already
        // be dead, so everything borrowed is shed rather than left
        // dangling. The shed shards' persisted bytes stay intact but their
        // manifests still read fresh — only the owner requeue rebuilds
        // their resident rows this session.
        LOG_ERROR("Index database growth failed: {}", grown.error());
        if(db.corrupted()) {
            recover_corrupt_database(report);
        } else {
            shed_borrowed_shards(report);
        }
        co_return;
    }
    bool grew = *grown;
    if(!grew) {
        // The LMDB backend hands out pointers into its resident read
        // snapshot; after a commit the resident shards migrate onto a
        // fresh snapshot so the old one can be retired. Both snapshots
        // stay valid across the yields and the blobs are byte-identical,
        // so queries between batches may observe a mix of old and new
        // pointers with identical meaning. Shards whose write just failed
        // are dirty again by now (their bytes never landed) and stay
        // memory-backed until a later save.
        auto advanced = db.advance_read_snapshot();
        if(!advanced) {
            LOG_WARN("Index read-snapshot advance failed: {}", advanced.error());
            if(db.corrupted()) {
                recover_corrupt_database(report);
            }
            co_return;
        }
        if(*advanced == 0) {
            // Filesystem backend: buffers are immortal, nothing to migrate.
            co_return;
        }
    }

    constexpr std::size_t rebind_batch = 512;
    llvm::SmallVector<std::uint32_t> resident;
    for(auto path_id: llvm::make_first_range(workspace.shards)) {
        if(!dirty_shards.contains(path_id)) {
            resident.push_back(path_id);
        }
    }
    for(std::size_t i = 0; i < resident.size(); i += 1) {
        if(!grew && i != 0 && i % rebind_batch == 0) {
            co_await kota::sleep(std::chrono::milliseconds(0), loop);
        }
        auto path_id = resident[i];
        auto it = workspace.shards.find(path_id);
        if(it == workspace.shards.end() || dirty_shards.contains(path_id)) {
            continue;
        }
        auto blob =
            db.read(index::IndexBlobKind::Shard, blob_key(workspace.path_pool.resolve(path_id)));
        if(!blob || !it->second.rebind(std::move(blob.buffer))) {
            // Corruption can also surface first here (a damaged page only
            // this re-read reaches); the recovery below sheds the whole
            // resident set, nothing per-shard to do.
            if(db.corrupted()) {
                break;
            }
            // Unreachable under the writer lock; the shard is dropped and
            // its owners requeued to rebuild the rows, while keeping the
            // old view would dangle once the snapshot retires.
            LOG_ERROR("Index shard for {} diverged during snapshot migration",
                      workspace.path_pool.resolve(path_id));
            assert(false && "persisted shard must survive snapshot migration");
            workspace.shards.erase(path_id);
            report.add_rows_changed(path_id);
            requeue_owners(path_id, report);
        }
    }
    // Corruption observed by any read since the write-time check — this
    // loop's, or a query's during its yields — condemns the database; the
    // recovery sheds every borrowed view before the environment closes.
    if(db.corrupted()) {
        recover_corrupt_database(report);
        co_return;
    }
    db.retire_old_snapshot();
}

void IndexStore::requeue_owners(std::uint32_t path_id, Report& report) {
    auto it = workspace.project_index.contributions.find(path_id);
    if(it == workspace.project_index.contributions.end()) {
        return;
    }
    for(auto tu: llvm::make_first_range(it->second)) {
        report.add_reindex(tu);
    }
}

void IndexStore::shed_borrowed_shards(Report& report) {
    llvm::SmallVector<std::uint32_t> shed;
    for(auto path_id: llvm::make_first_range(workspace.shards)) {
        if(!dirty_shards.contains(path_id)) {
            shed.push_back(path_id);
        }
    }
    for(auto path_id: shed) {
        workspace.shards.erase(path_id);
        report.add_rows_changed(path_id);
        requeue_owners(path_id, report);
    }
}

void IndexStore::recover_corrupt_database(Report& report) {
    LOG_WARN("Index database is corrupt; discarding it and rebuilding from scratch");
    saved_shards = 0;
    shed_borrowed_shards(report);
    for(auto tu_path_id: llvm::make_first_range(workspace.project_index.manifests)) {
        dirty_manifests.insert(tu_path_id);
    }
    global_dirty = true;
    cdb_dirty = true;
    // The snapshot just persisted (if any) predates this recovery's debt
    // and lives in a condemned database anyway: the caller's shutdown path
    // owes one metadata retry so the fresh database records it.
    report.snapshot_stale = true;
    persisted_cdb_snapshot.clear();
    reopen_fresh_database();
}

void IndexStore::reopen_fresh_database() {
    workspace.index_db->condemn();
    workspace.index_db.reset();
    workspace.index_db = index::open_database(*workspace.store, workspace.config.project.index_db);
}

IndexStore::LoadResult IndexStore::load(bool read_only) {
    LoadResult result;
    auto& report = result.report;
    if(!workspace.index_db)
        return result;
    auto& db = *workspace.index_db;
    auto& project = workspace.project_index;
    ScopedTimer timer;

    auto sweep_all = [&] {
        if(read_only) {
            return;
        }
        for(auto kind: {index::IndexBlobKind::Shard, index::IndexBlobKind::Manifest}) {
            db.for_each_key(kind, [&](llvm::StringRef key) {
                startup_removes.push_back({kind, key.str()});
            });
        }
    };

    auto global = db.read(index::IndexBlobKind::Global, "global");
    if(!global) {
        // A global blob that exists but failed to open is a transient IO
        // error, not absence: sweeping would destroy an intact index and
        // force a full rebuild. Run this session memory-only instead —
        // saving a fresh lineage over blobs whose anchor was never read
        // could alias their fv ids and generation stamps — and leave
        // everything for a healthier restart to load.
        if(db.contains(index::IndexBlobKind::Global, "global")) {
            // Confirmed page corruption heals through rebuildability: the
            // condemned database deletes itself and a fresh empty one
            // opens in its place, so this session's rebuild persists
            // instead of being redone at the next start. Transient
            // failures touch nothing.
            if(!read_only && db.corrupted()) {
                LOG_WARN("Index database is corrupt; discarding it and rebuilding from scratch");
                reopen_fresh_database();
            } else {
                LOG_WARN("Index global blob unreadable; disabling index persistence this session");
                workspace.index_db.reset();
            }
            return result;
        }
        // No global table means no resolvable manifests: everything else
        // is unreachable data, swept so it cannot survive as orphans.
        sweep_all();
        return result;
    }
    llvm::DenseMap<std::uint32_t, std::uint64_t> manifest_pins;
    if(!project.load_global(global.buffer->getBuffer(), workspace.path_pool, manifest_pins)) {
        LOG_INFO("Discarding old-format index global blob");
        sweep_all();
        if(!read_only) {
            startup_removes.push_back({index::IndexBlobKind::Global, "global"});
        }
        result.decoded = false;
        return result;
    }

    // Adopt exactly the manifests the global blob pins, at exactly the
    // pinned generation stamp and with every FileVersion resolvable. The
    // rest are stale residue — a crash between batch phases, a failed
    // write under a landed global, a dropped TU whose removal was lost —
    // and are swept, with their TUs re-enqueued where recoverable.
    llvm::DenseSet<std::uint32_t> adopted_pins;
    llvm::SmallVector<std::string> dead_manifests;
    db.for_each_key(index::IndexBlobKind::Manifest, [&](llvm::StringRef key) {
        auto blob = db.read(index::IndexBlobKind::Manifest, key);
        auto manifest = blob ? index::deserialize_manifest(blob.buffer->getBuffer()) : std::nullopt;
        auto pin = manifest ? manifest_pins.find(manifest->tu_fv) : manifest_pins.end();
        if(!manifest || pin == manifest_pins.end() || pin->second != manifest->global_gen ||
           !project.knows_file_versions(*manifest)) {
            dead_manifests.push_back(key.str());
            // The manifest raced a crash ahead of the global blob (its own
            // pin never landed). When the TU's version is still resolvable,
            // re-enqueue it: the CDB sweep never covers standalone-indexed
            // headers.
            if(manifest) {
                auto fv = project.file_versions.find(manifest->tu_fv);
                if(fv != project.file_versions.end()) {
                    report.add_reindex(fv->second.path_id);
                }
            }
            return;
        }
        adopted_pins.insert(manifest->tu_fv);
        auto tu_path_id = project.file_versions.find(manifest->tu_fv)->second.path_id;
        project.apply_manifest(tu_path_id, std::move(*manifest));
    });
    if(!read_only) {
        for(auto& key: dead_manifests) {
            startup_removes.push_back({index::IndexBlobKind::Manifest, std::move(key)});
        }
    }
    // A pinned TU without an adopted manifest lost it to a failed write
    // that the landed global outran, or to a lost removal; re-enqueue it —
    // its rows are unservable until a reindex. Pinned fvs always resolve:
    // load_global rejects a blob whose pins its own table cannot cover.
    for(auto fv: llvm::make_first_range(manifest_pins)) {
        if(!adopted_pins.contains(fv)) {
            report.add_reindex(project.file_versions.find(fv)->second.path_id);
        }
    }

    // Every contributing FileVersion pins the content generation its rows
    // were built from; the shard must store that same generation. The
    // variant check below cannot catch a stale shard alone when an edit
    // past every indexed row left the rows hash identical: all recorded
    // versions would match the disk while positions map through the old
    // text, forever. A version with no consumed-content hash (0) pins
    // nothing — it is permanently stale and reindexes its TU anyway.
    llvm::DenseMap<std::uint32_t, llvm::SmallVector<std::uint64_t, 1>> generations;
    for(auto& manifest: llvm::make_second_range(project.manifests)) {
        for(auto fv: llvm::make_first_range(manifest.contributions)) {
            auto& record = project.file_versions.find(fv)->second;
            if(record.content_hash == 0) {
                continue;
            }
            auto& pinned = generations[record.path_id];
            if(!llvm::is_contained(pinned, record.content_hash)) {
                pinned.push_back(record.content_hash);
            }
        }
    }

    // Fetch exactly the shard blobs the contributions expect. A blob that
    // is missing or fails verification leaves its contributing TUs' rows
    // unservable, so those manifests are dropped and the TUs reindex (for
    // headers no CDB entry would ever re-enqueue them otherwise).
    llvm::StringSet<> expected_keys;
    llvm::SmallVector<std::uint32_t> unservable;
    for(auto& [path_id, entry]: project.contributions) {
        auto key = blob_key(workspace.path_pool.resolve(path_id));
        auto shard = index::Shard::from_buffer(db.read(index::IndexBlobKind::Shard, key).buffer);
        // A blob can verify yet miss a contributed variant, or carry
        // another content generation than the contributions pin (crash or
        // failed write left a manifest newer than its shard); set_live
        // would drop missing rows silently and stale content misplaces
        // every position, so both are as unservable as an unreadable blob.
        auto generation_ok = [&] {
            auto it = generations.find(path_id);
            return it == generations.end() || llvm::all_of(it->second, [&](std::uint64_t hash) {
                       return hash == shard.content_hash();
                   });
        };
        bool servable = shard.loaded() && generation_ok() &&
                        llvm::all_of(llvm::make_second_range(entry),
                                     [&](std::uint64_t hash) { return shard.has_variant(hash); });
        if(!servable) {
            LOG_INFO("Discarding unservable shard for {}", workspace.path_pool.resolve(path_id));
            unservable.push_back(path_id);
            continue;
        }
        expected_keys.insert(key);
        shard.set_live(project.live_variants(path_id));
        workspace.shards[path_id] = std::move(shard);
    }
    llvm::SmallVector<std::uint32_t> mask_refresh;
    for(auto path_id: unservable) {
        auto contribution_it = project.contributions.find(path_id);
        if(contribution_it == project.contributions.end()) {
            continue;
        }
        llvm::SmallVector<std::uint32_t> owners;
        for(auto tu: llvm::make_first_range(contribution_it->second)) {
            owners.push_back(tu);
        }
        for(auto tu: owners) {
            // The removal retires the TU's contributions to EVERY file it
            // touched, not just the unservable one; the affected set feeds
            // the mask refresh below, like the merge path's.
            auto affected = project.remove_manifest(tu);
            mask_refresh.append(affected.begin(), affected.end());
            if(!read_only) {
                startup_removes.push_back(
                    {index::IndexBlobKind::Manifest, blob_key(workspace.path_pool.resolve(tu))});
            }
            report.add_reindex(tu);
        }
    }
    for(auto path_id: mask_refresh) {
        auto it = workspace.shards.find(path_id);
        if(it != workspace.shards.end()) {
            it->second.set_live(project.live_variants(path_id));
        }
    }

    // Sweep shard blobs nothing references any more.
    if(!read_only) {
        db.for_each_key(index::IndexBlobKind::Shard, [&](llvm::StringRef key) {
            if(!expected_keys.contains(key)) {
                startup_removes.push_back({index::IndexBlobKind::Shard, key.str()});
            }
        });
        reconcile_cdb_snapshot(report);
    }

    // The reads above touch every adopted manifest and shard, so page
    // corruption anywhere in the database has latched by now; heal it like
    // the unreadable-global case above instead of writing into a damaged
    // tree every session. The adopted state unwinds wholesale: the shard
    // views borrow from the condemned environment, and manifests kept
    // without their views would read as fresh and gate the rebuild sweep
    // off exactly the files whose rows were lost. Standalone-indexed TUs
    // re-enqueue first — the CDB sweep that rebuilds everything else never
    // covers them, and the condemned database is deleting their only
    // persistent record. The dirtied snapshot carries them as debt from
    // the fresh database's first save on, so even a crash before their
    // rebuild lands cannot lose them a second time.
    if(!read_only && db.corrupted()) {
        LOG_WARN("Index database is corrupt; discarding it and rebuilding from scratch");
        for(auto tu: llvm::make_first_range(project.manifests)) {
            if(!workspace.cdb.has_entry(workspace.path_pool.resolve(tu))) {
                report.add_reindex(tu);
            }
        }
        workspace.shards.clear();
        project = index::ProjectIndex();
        startup_removes.clear();
        persisted_cdb_snapshot.clear();
        cdb_dirty = true;
        reopen_fresh_database();
        return result;
    }

    if(!workspace.shards.empty()) {
        LOG_INFO("Loaded {} index shards, {} manifests, {} symbols",
                 workspace.shards.size(),
                 project.manifests.size(),
                 project.symbols.size());
    }
    LOG_PERF("startup",
             "phase=index_load symbols={} shards={} manifests={} elapsed_ms={}",
             project.symbols.size(),
             workspace.shards.size(),
             project.manifests.size(),
             timer.ms());
    return result;
}

llvm::SmallVector<std::uint32_t>
    IndexStore::standalone_of(llvm::ArrayRef<std::uint32_t> candidates) {
    llvm::SmallVector<std::uint32_t> debt;
    llvm::DenseSet<std::uint32_t> seen;
    for(auto id: candidates) {
        if(workspace.project_index.manifests.contains(id) ||
           workspace.cdb.has_entry(workspace.path_pool.resolve(id)) || !seen.insert(id).second) {
            continue;
        }
        debt.push_back(id);
    }
    return debt;
}

void IndexStore::reconcile_cdb_snapshot(Report& report) {
    auto blob = workspace.index_db->read(index::IndexBlobKind::CDB, "cdb");
    CDBSnapshot persisted;
    if(!blob ||
       !kota::codec::json::from_string(std::string_view(blob.buffer->getBuffer()), persisted)) {
        // Unknown baseline: nothing to diff against. Dirty the snapshot so
        // the next save recreates it even when it commits nothing else —
        // after a crash that lost only the CDB blob, waiting for an
        // unrelated dirtying merge would leave offline command edits
        // undetectable across every following session.
        cdb_dirty = true;
        return;
    }
    persisted_cdb_snapshot = blob.buffer->getBuffer().str();

    llvm::StringMap<const CDBSnapshotEntry*> before;
    for(auto& entry: persisted.entries) {
        before[entry.file] = &entry;
    }
    auto& project = workspace.project_index;
    llvm::DenseSet<std::uint32_t> cdb_ids;
    llvm::SmallVector<std::uint32_t> changed_ids;
    auto snapshot = build_cdb_snapshot(workspace, header_hosts, {});
    for(auto& entry: snapshot.entries) {
        if(entry.hashes.empty()) {
            continue;
        }
        auto server_id = workspace.path_pool.intern(entry.file);
        cdb_ids.insert(server_id);
        auto it = before.find(entry.file);
        // `selected` guards the offline winner flip: the candidate multiset
        // can survive a reload that still changes which entry is the
        // default selection.
        if(it != before.end() && it->second->hashes == entry.hashes &&
           it->second->selected == entry.selected && it->second->rules == entry.rules) {
            continue;
        }
        changed_ids.push_back(server_id);
        if(!project.manifests.contains(server_id)) {
            continue;
        }
        LOG_INFO("Compile command changed since the last session; reindexing {}", entry.file);
        drop_index_into(server_id, report);
        report.add_reindex(server_id);
    }

    // A standalone-indexed header borrowed a host source's command and
    // applied its own matched rules on top, so an offline change to either
    // staled its rows exactly like the live CDB path's hosted-header
    // invalidation. A header whose recorded host survives unchanged and
    // still includes it is pinned fresh; one with no recorded host (older
    // snapshot) falls back to the include-reachability approximation below.
    llvm::DenseSet<std::uint32_t> pinned_fresh;
    for(auto& entry: snapshot.entries) {
        if(!entry.hashes.empty()) {
            continue;
        }
        auto it = before.find(entry.file);
        if(it == before.end()) {
            // Not in the persisted snapshot: the next save adopts it, and
            // offline changes against an unknown baseline are undetectable
            // anyway.
            continue;
        }
        auto& old = *it->second;
        // The header's own CDB entry vanished: keep the index — last-known
        // content still serves navigation, mirroring the live treatment.
        if(!old.hashes.empty()) {
            continue;
        }
        auto server_id = workspace.path_pool.intern(entry.file);
        if(old.rules != entry.rules) {
            LOG_INFO("Config rules changed since the last session; reindexing {}", entry.file);
            drop_index_into(server_id, report);
            report.add_reindex(server_id);
            continue;
        }
        if(old.host.empty()) {
            continue;
        }
        auto host_id = workspace.path_pool.intern(old.host);
        if(!workspace.cdb.has_entry(old.host) || llvm::is_contained(changed_ids, host_id)) {
            LOG_INFO("Host compile command changed since the last session; reindexing {}",
                     entry.file);
            drop_index_into(server_id, report);
            report.add_reindex(server_id);
            continue;
        }
        // The dependency scan preceding this load saw the offline edits, so
        // a recorded host that no longer includes the header cannot vouch
        // for the borrowed command any more. Keep the rows serving (last
        // known good, like the vanished-entry case above) while a rebuild
        // re-selects a host; a Fallback resolution then changes nothing.
        // The retained rows were still built through the old host, so keep
        // that association until a landed rebuild overwrites it — an empty
        // host persisted after a Fallback or failed rebuild would hit the
        // `old.host.empty()` gate next session and never retry.
        if(workspace.dep_graph.find_include_chain(host_id, server_id).empty()) {
            LOG_INFO("Recorded host no longer includes {}; reindexing", entry.file);
            header_hosts[server_id] = host_id;
            report.add_reindex(server_id);
            continue;
        }
        header_hosts[server_id] = host_id;
        pinned_fresh.insert(server_id);
    }

    // A persisted standalone entry whose file has no manifest is recorded
    // debt: its index was dropped for a command or rule change and no
    // rebuild has landed since — with no manifest pin and no CDB entry,
    // nothing else would ever retry it. Re-enqueue while the file exists;
    // a vanished file's debt dies with its entry at the next save.
    for(auto& old: persisted.entries) {
        if(!old.hashes.empty() || workspace.cdb.has_entry(old.file)) {
            continue;
        }
        auto server_id = workspace.path_pool.intern(old.file);
        if(project.manifests.contains(server_id) || !fs::exists(old.file)) {
            continue;
        }
        LOG_INFO("Index owed from the last session; reindexing {}", old.file);
        report.add_reindex(server_id);
    }
    if(changed_ids.empty()) {
        return;
    }

    llvm::SmallVector<std::uint32_t> hosted;
    for(auto tu: llvm::make_first_range(project.manifests)) {
        if(cdb_ids.contains(tu) || pinned_fresh.contains(tu)) {
            continue;
        }
        if(llvm::any_of(changed_ids, [&](std::uint32_t host) {
               return !workspace.dep_graph.find_include_chain(host, tu).empty();
           })) {
            hosted.push_back(tu);
        }
    }
    for(auto header_id: hosted) {
        LOG_INFO("Host compile command changed since the last session; reindexing {}",
                 workspace.path_pool.resolve(header_id));
        drop_index_into(header_id, report);
        report.add_reindex(header_id);
    }
}

bool IndexStore::file_version_stale(std::uint32_t fv_id) {
    auto [cached, inserted] = fv_verdicts.try_emplace(fv_id, true);
    if(!inserted) {
        return cached->second;
    }

    auto record_it = workspace.project_index.file_versions.find(fv_id);
    if(record_it == workspace.project_index.file_versions.end()) {
        return true;
    }
    auto& record = record_it->second;
    auto path = workspace.path_pool.resolve(record.path_id);

    // Two-layer test on the shared FileVersion: Layer 1 trusts a stat EQUAL
    // to the recorded stamp (no file read) — equality, not a watermark, so
    // backdated or preserved mtimes cannot masquerade as fresh; Layer 2
    // re-hashes the disk against the consumed-content hash and treats a
    // match as a mere touch, repairing the stamp in place — once, for every
    // TU that consumes this version.
    auto stale = [&] {
        fs::file_status status;
        if(auto err = fs::status(path, status)) {
            return true;
        }
        if(record.mtime_ns != 0 && record.size == status.getSize() &&
           record.mtime_ns == fs::mtime_ns(status)) {
            return false;
        }
        if(record.content_hash == 0) {
            return true;
        }
        if(hash_file(path) != record.content_hash) {
            return true;
        }
        record.size = status.getSize();
        record.mtime_ns = fs::mtime_ns(status);
        global_dirty = true;
        return false;
    }();
    fv_verdicts[fv_id] = stale;
    return stale;
}

bool IndexStore::need_update(llvm::StringRef file_path) {
    auto& project = workspace.project_index;
    auto manifest_it = project.manifests.find(workspace.path_pool.intern(file_path));
    if(manifest_it == project.manifests.end())
        return true;

    // Every referenced version must be validated: whichever a partial
    // iteration skipped would keep serving stale rows behind a fresh
    // verdict.
    auto& manifest = manifest_it->second;
    if(file_version_stale(manifest.tu_fv)) {
        return true;
    }
    for(auto& node: manifest.nodes) {
        if(file_version_stale(node.fv)) {
            return true;
        }
    }
    return false;
}

}  // namespace clice
