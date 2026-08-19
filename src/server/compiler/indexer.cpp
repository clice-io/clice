#include "server/compiler/indexer.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "index/manifest.h"
#include "index/shard.h"
#include "index/storage.h"
#include "index/tu_index.h"
#include "server/compiler/context_resolver.h"
#include "server/protocol/worker.h"
#include "server/state/session_store.h"
#include "server/worker/worker_pool.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"

#include "kota/codec/json/json.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
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

/// JSON layout of the persisted CDB snapshot (blob kind Cdb): per source
/// file, the sorted canonical command hashes of its entries and a hash of
/// its matched config rules when the index state was last saved. A
/// standalone-indexed header gets an entry too (empty hashes): its own
/// matched rules plus the host source whose command its rows borrowed.
struct CdbSnapshotEntry {
    std::string file;
    std::vector<std::string> hashes;
    std::string rules;
    std::string host;
};

struct CdbSnapshot {
    std::vector<CdbSnapshotEntry> entries;
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

CdbSnapshot build_cdb_snapshot(Workspace& workspace,
                               const llvm::DenseMap<std::uint32_t, std::uint32_t>& header_hosts,
                               llvm::ArrayRef<std::uint32_t> standalone_debt) {
    CdbSnapshot snapshot;
    for(auto& [path_id, hashes]: workspace.cdb.command_hash_snapshot()) {
        auto file = workspace.cdb.resolve_path(path_id).str();
        auto rules = rules_hash(workspace.config, file);
        snapshot.entries.push_back({
            .file = std::move(file),
            .hashes = {hashes.begin(), hashes.end()},
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
    std::ranges::sort(snapshot.entries, {}, &CdbSnapshotEntry::file);
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

bool Indexer::merge(const void* tu_index_data, std::size_t size) {
    // Zero-copy consumption: the wire stays serialized; a new variant's
    // blob bytes are sliced out and installed or merged without decoding
    // the envelope, and only genuinely new symbol names are materialized.
    auto view =
        index::TUIndex::from_bytes(llvm::StringRef(static_cast<const char*>(tu_index_data), size));
    if(!view.loaded()) {
        LOG_WARN("Ignoring TUIndex that failed verification");
        return false;
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
                return false;
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
            return false;
        }
        auto fresh = index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
        if(!fresh.loaded()) {
            LOG_WARN("Reject merge for {}: rows for {} do not form a valid shard",
                     main_tu_path,
                     workspace.path_pool.resolve(global_id));
            return false;
        }
        if(!record_consumed(local_id, fresh.content_hash())) {
            return false;
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
        return false;
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

    for(auto& [global_id, replacement]: replacements) {
        workspace.shards[global_id] = std::move(replacement);
        dirty_shards.insert(global_id);
    }

    // Replace this TU's manifest wholesale: files it no longer touches lose
    // their contribution here, which is also what retires their variants —
    // no sweep over other shards is needed.
    auto affected = project.apply_manifest(tu_path_id, std::move(manifest));
    for(auto path_id: affected) {
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
                enqueue(tu, ReindexReason::ContentChanged);
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
    return true;
}

void Indexer::drop_index(std::uint32_t tu_path_id) {
    auto& project = workspace.project_index;
    if(!project.manifests.contains(tu_path_id)) {
        return;
    }
    for(auto path_id: project.remove_manifest(tu_path_id)) {
        auto it = workspace.shards.find(path_id);
        if(it != workspace.shards.end()) {
            it->second.set_live(project.live_variants(path_id));
        }
    }
    dirty_manifests.insert(tu_path_id);
    global_dirty = true;
}

kota::task<> Indexer::save() {
    // Reset up front: every early return below means this save committed
    // nothing, and the gauge must not keep exposing the previous round's
    // count as current.
    saved_shards = 0;
    if(!workspace.index_storage)
        co_return;
    auto& storage = *workspace.index_storage;
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
        auto it = project.contributions.find(path_id);
        if(it == project.contributions.end()) {
            continue;
        }
        for(auto tu: llvm::make_first_range(it->second)) {
            enqueue(tu, ReindexReason::ContentChanged);
        }
    }

    // Snapshot the dirty state on the loop: everything below serializes
    // from copies, so merges landing across the write await simply re-dirty
    // for the next save. The id vectors parallel the batch so a failed
    // entry can be re-dirtied by its batch index.
    std::vector<index::IndexStorage::Blob> batch;
    llvm::SmallVector<std::uint32_t> shard_ids;
    llvm::SmallVector<std::uint32_t> manifest_ids;
    llvm::SmallVector<std::pair<index::IndexBlobKind, std::string>> removals;
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
    std::string cdb_bytes;
    std::optional<std::size_t> cdb_index;
    if(!batch.empty() || !removals.empty() || cdb_dirty) {
        cdb_bytes = serialize_cdb_snapshot(workspace, header_hosts, standalone_debt());
        if(!cdb_bytes.empty() && cdb_bytes != persisted_cdb_snapshot) {
            cdb_index = batch.size();
            batch.push_back({index::IndexBlobKind::Cdb, "cdb", cdb_bytes});
        } else {
            cdb_dirty = false;
        }
    }

    dirty_shards.clear();
    dirty_manifests.clear();
    global_dirty = false;

    if(batch.empty() && removals.empty()) {
        co_return;
    }

    // The dirty set was snapshot-cleared above so merges landing across
    // the write await re-dirty for the next save; the in-flight count
    // keeps pending_shard_writes() truthful meanwhile — a stats reader
    // polling for "shard writes settled" must not observe zero while the
    // commit is still running (and saved_shards still holds its reset).
    saving_shards = shard_count;
    llvm::SmallVector<std::size_t> failed;
    co_await kota::queue([&] {
        failed = storage.write(batch);
        for(auto& [kind, key]: removals) {
            storage.remove(kind, key);
        }
    });
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

    LOG_PERF("index",
             "phase=save shards={} manifests={} total={} elapsed_ms={}",
             shard_count,
             manifest_count,
             workspace.shards.size(),
             timer.ms());
}

bool Indexer::load(bool read_only) {
    if(!workspace.index_storage)
        return true;
    auto& storage = *workspace.index_storage;
    auto& project = workspace.project_index;
    ScopedTimer timer;

    auto sweep_all = [&] {
        if(read_only) {
            return;
        }
        for(auto kind: {index::IndexBlobKind::Shard, index::IndexBlobKind::Manifest}) {
            llvm::SmallVector<std::string> keys;
            storage.for_each_key(kind, [&](llvm::StringRef key) { keys.push_back(key.str()); });
            for(auto& key: keys) {
                storage.remove(kind, key);
            }
        }
    };

    auto global = storage.read(index::IndexBlobKind::Global, "global");
    if(!global) {
        // A global blob that exists but failed to open is a transient IO
        // error, not absence: sweeping would destroy an intact index and
        // force a full rebuild. Run this session memory-only instead —
        // saving a fresh lineage over blobs whose anchor was never read
        // could alias their fv ids and generation stamps — and leave
        // everything for a healthier restart to load.
        if(storage.contains(index::IndexBlobKind::Global, "global")) {
            LOG_WARN("Index global blob unreadable; disabling index persistence this session");
            workspace.index_storage.reset();
            return true;
        }
        // No global table means no resolvable manifests: everything else
        // is unreachable data, swept so it cannot survive as orphans.
        sweep_all();
        return true;
    }
    llvm::DenseMap<std::uint32_t, std::uint64_t> manifest_pins;
    if(!project.load_global(global->getBuffer(), workspace.path_pool, manifest_pins)) {
        LOG_INFO("Discarding old-format index global blob");
        sweep_all();
        if(!read_only) {
            storage.remove(index::IndexBlobKind::Global, "global");
        }
        return false;
    }

    // Adopt exactly the manifests the global blob pins, at exactly the
    // pinned generation stamp and with every FileVersion resolvable. The
    // rest are stale residue — a crash between batch phases, a failed
    // write under a landed global, a dropped TU whose removal was lost —
    // and are swept, with their TUs re-enqueued where recoverable.
    llvm::DenseSet<std::uint32_t> adopted_pins;
    llvm::SmallVector<std::string> dead_manifests;
    storage.for_each_key(index::IndexBlobKind::Manifest, [&](llvm::StringRef key) {
        auto blob = storage.read(index::IndexBlobKind::Manifest, key);
        auto manifest = blob ? index::deserialize_manifest(blob->getBuffer()) : std::nullopt;
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
                    enqueue(fv->second.path_id, ReindexReason::ContentChanged);
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
            storage.remove(index::IndexBlobKind::Manifest, key);
        }
    }
    // A pinned TU without an adopted manifest lost it to a failed write
    // that the landed global outran, or to a lost removal; re-enqueue it —
    // its rows are unservable until a reindex. Pinned fvs always resolve:
    // load_global rejects a blob whose pins its own table cannot cover.
    for(auto fv: llvm::make_first_range(manifest_pins)) {
        if(!adopted_pins.contains(fv)) {
            enqueue(project.file_versions.find(fv)->second.path_id, ReindexReason::ContentChanged);
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
        auto shard = index::Shard::from_buffer(storage.read(index::IndexBlobKind::Shard, key));
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
                storage.remove(index::IndexBlobKind::Manifest,
                               blob_key(workspace.path_pool.resolve(tu)));
            }
            enqueue(tu, ReindexReason::ContentChanged);
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
        llvm::SmallVector<std::string> orphans;
        storage.for_each_key(index::IndexBlobKind::Shard, [&](llvm::StringRef key) {
            if(!expected_keys.contains(key)) {
                orphans.push_back(key.str());
            }
        });
        for(auto& key: orphans) {
            storage.remove(index::IndexBlobKind::Shard, key);
        }
        reconcile_cdb_snapshot();
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
    return true;
}

llvm::SmallVector<std::uint32_t> Indexer::standalone_debt() {
    llvm::SmallVector<std::uint32_t> debt;
    llvm::DenseSet<std::uint32_t> seen;
    auto add = [&](std::uint32_t id) {
        if(workspace.project_index.manifests.contains(id) ||
           workspace.cdb.has_entry(workspace.path_pool.resolve(id)) || !seen.insert(id).second) {
            return;
        }
        debt.push_back(id);
    };
    for(auto id: failed_ids) {
        add(id);
    }
    for(auto id: llvm::make_first_range(reindex_reasons)) {
        add(id);
    }
    return debt;
}

void Indexer::reconcile_cdb_snapshot() {
    auto blob = workspace.index_storage->read(index::IndexBlobKind::Cdb, "cdb");
    CdbSnapshot persisted;
    if(!blob || !kota::codec::json::from_string(std::string_view(blob->getBuffer()), persisted)) {
        // Unknown baseline: nothing to diff against. Dirty the snapshot so
        // the next save recreates it even when it commits nothing else —
        // after a crash that lost only the CDB blob, waiting for an
        // unrelated dirtying merge would leave offline command edits
        // undetectable across every following session.
        cdb_dirty = true;
        return;
    }
    persisted_cdb_snapshot = blob->getBuffer().str();

    llvm::StringMap<const CdbSnapshotEntry*> before;
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
        if(it != before.end() && it->second->hashes == entry.hashes &&
           it->second->rules == entry.rules) {
            continue;
        }
        changed_ids.push_back(server_id);
        if(!project.manifests.contains(server_id)) {
            continue;
        }
        LOG_INFO("Compile command changed since the last session; reindexing {}", entry.file);
        drop_index(server_id);
        enqueue(server_id, ReindexReason::ContentChanged);
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
            drop_index(server_id);
            enqueue(server_id, ReindexReason::ContentChanged);
            continue;
        }
        if(old.host.empty()) {
            continue;
        }
        auto host_id = workspace.path_pool.intern(old.host);
        if(!workspace.cdb.has_entry(old.host) || llvm::is_contained(changed_ids, host_id)) {
            LOG_INFO("Host compile command changed since the last session; reindexing {}",
                     entry.file);
            drop_index(server_id);
            enqueue(server_id, ReindexReason::ContentChanged);
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
            enqueue(server_id, ReindexReason::ContentChanged);
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
        if(project.manifests.contains(server_id) || reindex_reasons.contains(server_id) ||
           !fs::exists(old.file)) {
            continue;
        }
        LOG_INFO("Index owed from the last session; reindexing {}", old.file);
        enqueue(server_id, ReindexReason::ContentChanged);
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
        drop_index(header_id);
        enqueue(header_id, ReindexReason::ContentChanged);
    }
}

bool Indexer::file_version_stale(std::uint32_t fv_id) {
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

bool Indexer::need_update(llvm::StringRef file_path) {
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

void Indexer::enqueue(std::uint32_t server_path_id, ReindexReason reason) {
    // A fresh slot means any prior slot was already consumed (or none
    // existed); a queued-and-unconsumed slot makes this call a duplicate.
    bool fresh_slot = pending_ids.insert(server_path_id).second;

    // Record (or refresh) why the file is pending. Within one queued slot
    // ContentChanged is absorbing: a deps-only cascade cannot downgrade a
    // file whose own content already changed. Across slots it is not: a
    // deps-only requeue after the previous slot was consumed is new debt of
    // its own kind — the in-flight (or finished) pass already covers the
    // earlier content change, and keeping ContentChanged would suppress the
    // file's rows past that pass. The fresh ticket invalidates the clear of
    // any index task already in flight for this file.
    ++reindex_ticket;
    auto [it, inserted] =
        reindex_reasons.try_emplace(server_path_id,
                                    reason,
                                    reindex_ticket,
                                    reason == ReindexReason::ContentChanged ? reindex_ticket : 0);
    if(!inserted) {
        if(reason == ReindexReason::ContentChanged) {
            it->second.reason = ReindexReason::ContentChanged;
            it->second.content_ticket = reindex_ticket;
            // New content starts a fresh poison budget: the crashes the
            // old bytes caused say nothing about the fixed ones, and a
            // stale ledger would abandon the file on its first hiccup.
            it->second.requeue_attempts = 0;
        } else if(fresh_slot) {
            it->second.reason = ReindexReason::DepsOnly;
        }
        it->second.ticket = reindex_ticket;
    }

    if(!fresh_slot)
        return;
    index_queue.push_back(server_path_id);
}

void Indexer::pause_indexing() {
    ++pause_depth;
    if(pause_depth == 1) {
        resume_event.reset();
        LOG_DEBUG("Background indexing paused");
    }
}

void Indexer::resume_indexing() {
    if(pause_depth > 0)
        --pause_depth;
    if(pause_depth == 0) {
        resume_event.set();
        LOG_DEBUG("Background indexing resumed");
    }
}

kota::task<> Indexer::stop() {
    bg_tasks.cancel();
    co_await bg_tasks.join();
}

void Indexer::schedule() {
    if(!workspace.config.project.enable_indexing.value || indexing_active || indexing_scheduled)
        return;
    indexing_scheduled = true;

    if(!index_idle_timer) {
        index_idle_timer = std::make_shared<kota::timer>(kota::timer::create(loop));
    }
    index_idle_timer->start(
        std::chrono::milliseconds(workspace.config.project.idle_timeout_ms.value));

    if(!bg_tasks.spawn(run_background_indexing())) {
        indexing_scheduled = false;
        LOG_WARN("Failed to spawn background indexing task (task group stopped)");
    }
}

kota::task<> Indexer::index_one(std::uint32_t server_path_id,
                                std::uint64_t ticket,
                                std::size_t index,
                                std::size_t total) {
    auto file_path = std::string(workspace.path_pool.resolve(server_path_id));

    // Open files are skipped until an agent shows up: the LSP side never
    // reads their shards (sessions serve them), so indexing them is pure
    // waste — but agents read disk truth and need the shards, snapshot
    // taken from disk regardless of the live buffer. Skipping loses no
    // debt: BufferClosed re-checks the shard against the disk on close.
    if(!index_open_files && sessions.find(server_path_id) != nullptr)
        co_return;

    // The engine's own observation is authoritative for content changes:
    // it saw the event. The dep-hash check below cannot be trusted to see
    // a file's own edit (it validates the recorded dependencies), so only
    // deps-only slots — where it exists to deduplicate cascade storms —
    // may take the shortcut.
    if(auto it = reindex_reasons.find(server_path_id);
       (it == reindex_reasons.end() || it->second.reason != ReindexReason::ContentChanged) &&
       !need_update(file_path)) {
        co_return;
    }

    // For module interface units, compile their PCM (and transitive deps)
    // first so the stateless worker has the artifacts it needs.
    if(workspace.compile_graph && workspace.path_to_module.contains(server_path_id)) {
        co_await workspace.compile_graph->compile(server_path_id);
    }

    worker::BuildParams params;
    params.kind = worker::BuildKind::Index;
    params.file = file_path;
    // Bulk background indexing sticks to real commands; synthesized fallback
    // commands would fill the index with guesses.
    std::uint32_t host_path_id = no_path_id;
    auto source = contexts.resolve_command(file_path,
                                           params.directory,
                                           params.arguments,
                                           nullptr,
                                           &host_path_id);
    if(source == CommandSource::Fallback) {
        // A file whose manifest survives keeps serving its last-known rows,
        // so skipping it loses nothing. One without a manifest (dropped or
        // never built) stays unindexed — count that as a failure so a batch
        // run reports the debt instead of exiting clean.
        if(!workspace.project_index.manifests.contains(server_path_id)) {
            LOG_WARN("[{}/{}] No compile command found for {}; it stays unindexed",
                     index,
                     total,
                     file_path);
            failed_ids.insert(server_path_id);
        }
        co_return;
    }

    workspace.fill_pcm_deps(params.pcms);

    LOG_INFO("[{}/{}] Indexing {}", index, total, file_path);

    ScopedTimer timer;
    auto result = co_await pool.send_stateless(params);
    if(result.has_value() && result.value().success && !result.value().tu_index_data.empty()) {
        auto index_ms = timer.ms();
        // Merge guard: a newer content-level invalidation during this build
        // (or a removal clearing the entry) means this result describes text
        // that no longer exists — e.g. a compile-command change whose
        // erase+re-enqueue must not be undone by an in-flight merge of the
        // old-command rows. Drop the merge; the follow-up slot redoes it.
        // A deps-only requeue is deliberately NOT superseding: the in-flight
        // rows are positionally right, and suppressing them would trade a
        // tolerated semantic drift for a coverage hole.
        if(auto it = reindex_reasons.find(server_path_id);
           it == reindex_reasons.end() || it->second.content_ticket > ticket) {
            LOG_INFO("Discarding superseded index result for {}", file_path);
            co_return;
        }
        ScopedTimer merge_timer;
        if(!merge(result.value().tu_index_data.data(), result.value().tu_index_data.size())) {
            // Rejected wholesale: the file's rows are missing or stale,
            // which is a failure, not a completed index.
            failed_ids.insert(server_path_id);
            co_return;
        }
        failed_ids.erase(server_path_id);
        // Record the borrowed host only for rows that landed: written at
        // dispatch, a failed rebuild would leave the persisted CDB
        // snapshot naming the new host while the retained rows were built
        // through the old one — an unchanged new host then pins those
        // stale rows fresh across restarts.
        if(source == CommandSource::IncludeGraph) {
            header_hosts[server_path_id] = host_path_id;
        }
        LOG_PERF("index",
                 "progress={}/{} file={} bytes={} index_ms={} merge_ms={}",
                 index,
                 total,
                 file_path,
                 result.value().tu_index_data.size(),
                 index_ms,
                 merge_timer.ms());
    } else if(result.has_value() && !result.value().success) {
        LOG_WARN("[{}/{}] Index failed for {}: {}", index, total, file_path, result.value().error);
        failed_ids.insert(server_path_id);
    } else if(result.has_value() && result.value().tu_index_data.empty()) {
        LOG_WARN("[{}/{}] Index returned empty TUIndex for {}", index, total, file_path);
        failed_ids.insert(server_path_id);
    } else if(result.error().code == worker::dispatch_errc::cancelled ||
              result.error().code == worker::dispatch_errc::worker_crashed ||
              (result.error().code == worker::dispatch_errc::worker_unavailable &&
               pool.revives_slots())) {
        // Preempted under memory pressure or lost to a worker crash: the
        // work itself is fine — requeue the file with its original reason so
        // the next round redoes it instead of silently dropping coverage.
        // worker_unavailable requeues (budget-free, like a preemption) only
        // when the pool revives dead slots: the outage is then a window, not
        // a verdict, and each retry round waits out the idle timer rather
        // than spinning. Without revival a requeue could never succeed.
        bool crashed = result.error().code == worker::dispatch_errc::worker_crashed;
        switch(note_dispatch_failure(server_path_id, ticket, crashed)) {
            case RequeueVerdict::Dropped: {
                LOG_INFO("[{}/{}] Index dropped for removed file {}", index, total, file_path);
                break;
            }
            case RequeueVerdict::Superseded: {
                LOG_INFO("[{}/{}] Index failure for superseded content of {}",
                         index,
                         total,
                         file_path);
                break;
            }
            case RequeueVerdict::GaveUp: {
                // Log-only by design: the file is usually not open (open
                // documents are served by their session, not the shard),
                // so there is no diagnostic surface. Cross-file references
                // into this file stay stale until its content changes.
                LOG_WARN(
                    "[{}/{}] Index giving up on {} after {} crash requeues; "
                    "its cross-file data stays stale until it is edited: {}",
                    index,
                    total,
                    file_path,
                    max_requeue_attempts,
                    result.error().message);
                failed_ids.insert(server_path_id);
                break;
            }
            case RequeueVerdict::Requeued: {
                LOG_INFO("[{}/{}] Index requeued for {}: {}",
                         index,
                         total,
                         file_path,
                         result.error().message);
                break;
            }
        }
    } else {
        LOG_WARN("[{}/{}] Index IPC error for {}: {}",
                 index,
                 total,
                 file_path,
                 result.error().message);
        failed_ids.insert(server_path_id);
    }
}

auto Indexer::note_dispatch_failure(std::uint32_t server_path_id,
                                    std::uint64_t ticket,
                                    bool crashed) -> RequeueVerdict {
    // Only while the pending entry survives: a file removed from disk
    // mid-flight was cleared and has nothing to redo.
    auto it = reindex_reasons.find(server_path_id);
    if(it == reindex_reasons.end()) {
        return RequeueVerdict::Dropped;
    }

    // The failed dispatch carried bytes a ContentChanged enqueue has since
    // replaced: its crash says nothing about the fixed content, so it
    // neither spends the fresh budget nor requeues — the newer content's
    // own slot redoes the work.
    if(it->second.content_ticket > ticket) {
        return RequeueVerdict::Superseded;
    }

    // The budget both counts and gates crashes only: a preemption under
    // memory pressure says nothing about the file, so it requeues even
    // when the crash budget is already spent — giving up on it would
    // clear the pending state and serve the stale shard as fresh.
    if(crashed) {
        if(it->second.requeue_attempts >= max_requeue_attempts) {
            // Giving up accepts the staleness, so clear the pending slot
            // here rather than relying on run_index_task's ticket-guarded
            // clear: a deps-only enqueue that landed mid-flight bumped the
            // ticket, and the guard would leave that downgraded entry
            // queued — a doomed retry that spends one more worker.
            clear_pending(server_path_id);
            return RequeueVerdict::GaveUp;
        }
        it->second.requeue_attempts += 1;
    }
    // Requeue with the debt class this dispatch carried, not the entry's
    // current one: a deps-only enqueue that landed mid-flight downgraded
    // the reason betting on this content pass to land — a failed pass
    // leaves the edit uncovered, and only a ContentChanged pending entry
    // keeps suppressing the stale shard.
    auto reason =
        it->second.content_ticket == ticket ? ReindexReason::ContentChanged : it->second.reason;
    // The enqueue bumps the entry's ticket, which shields it from the
    // in-flight task's pending-state clear. It also resets the poison
    // budget on ContentChanged — right for a user edit, wrong for this
    // requeue of the same bytes — so restore the ledger afterwards
    // (try_emplace on the existing key keeps `it` valid).
    auto attempts = it->second.requeue_attempts;
    enqueue(server_path_id, reason);
    it->second.requeue_attempts = attempts;
    return RequeueVerdict::Requeued;
}

kota::task<> Indexer::run_index_task(std::uint32_t server_path_id,
                                     std::uint64_t ticket,
                                     std::size_t index,
                                     std::size_t total,
                                     std::size_t& completed) {
    co_await index_one(server_path_id, ticket, index, total);
    // The pending window ends with the index attempt, success or not. On
    // failure the last-known rows resume serving — deliberately: keeping
    // the gate would hide a file that fails to index (broken compile,
    // missing command) from every cross-file query with no recovery path,
    // since only a future event re-enqueues it. Any such event re-judges
    // staleness by content hash. A re-enqueue during the flight bumped
    // the ticket: that newer pending state must survive this clear.
    if(auto it = reindex_reasons.find(server_path_id);
       it != reindex_reasons.end() && it->second.ticket == ticket) {
        reindex_reasons.erase(it);
    }
    ++completed;
    progress_data.stage = Progress::Stage::Report;
    progress_data.completed = completed;
    on_progress_changed.emit();
}

kota::task<> Indexer::run_background_indexing() {
    if(index_idle_timer) {
        co_await index_idle_timer->wait();
    }
    indexing_scheduled = false;

    if(index_queue_pos >= index_queue.size()) {
        LOG_DEBUG("Background indexing: queue exhausted");
        co_return;
    }

    indexing_active = true;
    LOG_DEBUG("Background indexing: starting, {} files queued",
              index_queue.size() - index_queue_pos);

    // FileVersion verdicts hold for one round: the disk can change under a
    // running round, but staleness is re-judged per round anyway.
    fv_verdicts.clear();

    std::stable_partition(
        index_queue.begin() + index_queue_pos,
        index_queue.end(),
        [this](std::uint32_t id) { return workspace.path_to_module.contains(id); });

    auto total = index_queue.size() - index_queue_pos;
    std::size_t dispatched = 0;
    std::size_t completed = 0;

    // Announce the round; a progress reporter reads the counts from
    // progress() and owns the LSP token's begin/report/end handshake. With
    // no subscriber the signal is simply a no-op.
    progress_data = Progress{.stage = Progress::Stage::Begin, .total = total};
    on_progress_changed.emit();

    // Timed at the start of real work; the reporter's token handshake runs
    // off to the side and cannot inflate the reported indexing duration.
    ScopedTimer timer;
    kota::task_group<> workers(loop);

    while(index_queue_pos < index_queue.size()) {
        if(pause_depth > 0)
            co_await resume_event.wait();

        auto server_path_id = index_queue[index_queue_pos++];
        pending_ids.erase(server_path_id);
        // No open-session or hash-freshness shortcut here: index_one is the
        // single decision point for skipping (it knows the pending reason;
        // a hash check alone cannot see a file's own edit), and the
        // completion clear in run_index_task retires the pending state with
        // the ticket honored. A second, reason-blind copy of these checks
        // here is exactly what once erased ContentChanged state early and
        // let a stale shard keep serving.

        // A queued slot with no pending entry was cleared mid-batch: the
        // file was removed from disk after being enqueued (clear_pending),
        // so there is nothing to index — skip the slot. Every other slot
        // has an entry, because enqueue writes it before the queue push.
        auto pending_it = reindex_reasons.find(server_path_id);
        if(pending_it == reindex_reasons.end()) {
            continue;
        }

        ++dispatched;
        auto ticket = pending_it->second.ticket;
        // A member coroutine, not an immediately-invoked capturing lambda:
        // a lambda's captures live in the lambda object, which dies at the
        // end of this statement — anything read after the first suspension
        // would dangle. Coroutine parameters are copied into the frame.
        workers.spawn(run_index_task(server_path_id, ticket, dispatched, total, completed));
    }

    LOG_DEBUG("Background indexing: all {} tasks spawned, waiting for completion", dispatched);
    co_await workers.join();

    // Skipped files bump `completed` without a Report emit; refresh the
    // materialized count so a subscriber waking up on End reads the truth.
    progress_data.completed = completed;
    progress_data.stage = Progress::Stage::End;
    progress_data.dispatched = dispatched;
    on_progress_changed.emit();

    // Safe point to compact: no dispatch loop holds an index into the queue.
    // Files enqueued while we awaited the workers keep the queue alive for
    // the next scheduled round.
    if(index_queue_pos >= index_queue.size()) {
        assert(pending_ids.empty() && "drained queue must have no pending ids");
        assert(reindex_reasons.empty() && "drained queue must have no pending reasons");
        index_queue.clear();
        index_queue_pos = 0;
    }

    LOG_PERF("index",
             "phase=run dispatched={} skipped={} total={} elapsed_ms={}",
             dispatched,
             total - dispatched,
             total,
             timer.ms());
    co_await save();

    // The round owns the "active" gate through its save: releasing it
    // before the write await would let a next round's save overlap this
    // one's in-flight batch, racing same-key blob writes on the pool.
    indexing_active = false;

    // Files enqueued while the round was joining its workers saw their
    // schedule() no-op against indexing_active; without this kick they
    // would wait for the next external event — and a content-changed
    // pending file's rows stay skipped for that whole wait.
    if(index_queue_pos < index_queue.size()) {
        schedule();
    }
}

}  // namespace clice
