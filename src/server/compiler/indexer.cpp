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

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice {

/// Stable blob key for a file's shard or a TU's manifest: runtime pool ids
/// are per-session, so blobs are named by a hash of the path instead.
static std::string blob_key(llvm::StringRef path) {
    return std::format("{:016x}", llvm::xxh3_64bits(path));
}

void Indexer::merge(const void* tu_index_data, std::size_t size) {
    // Zero-copy consumption: the wire stays serialized; only miss sections
    // and genuinely new symbol names are ever materialized.
    auto loaded =
        index::TUIndexView::from(llvm::StringRef(static_cast<const char*>(tu_index_data), size));
    if(!loaded) {
        LOG_WARN("Ignoring TUIndex that failed verification");
        return;
    }
    auto& view = *loaded;
    if(view.path_count() == 0) {
        LOG_WARN("Ignoring TUIndex with empty path graph");
        return;
    }
    auto main_local_id = view.path_count() - 1;
    llvm::StringRef main_tu_path = view.path(main_local_id);

    // Shards pair the worker's rows with content read from disk here; if the
    // disk moved on since the worker read it, the rows' offsets describe
    // bytes that no longer exist and merging would misplace every position
    // until the next reindex. The worker's consumed-content hash arbitrates.
    // A missing hash (0) proceeds as before.
    auto content_matches = [&](std::uint32_t local_id, llvm::StringRef disk_content) {
        auto consumed = view.path_hash(local_id);
        return consumed == 0 || llvm::xxh3_64bits(disk_content) == consumed;
    };

    // The main file's verdict gates the WHOLE result: every section and the
    // manifest describe this one compile, so applying any of it against a
    // moved-on main file would mix two generations. Skipping everything
    // keeps the last-known state consistent; the changed file fails the
    // next staleness check (or is already pending), and a follow-up pass
    // redoes the merge against settled content.
    auto main_buf = llvm::MemoryBuffer::getFile(main_tu_path);
    if(!main_buf) {
        LOG_WARN("Skip merge for {}: cannot read content: {}",
                 main_tu_path,
                 main_buf.getError().message());
        return;
    }
    if(!content_matches(main_local_id, (*main_buf)->getBuffer())) {
        LOG_INFO("Skip merge for {}: disk moved on since it was indexed", main_tu_path);
        return;
    }

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

    // The previous contribution entry for a file this pass must skip (disk
    // moved on under a section): its rows stay consistent with the shard
    // they live in, so it keeps serving; a later pass redoes the file.
    auto carry_old_contribution = [&](std::uint32_t global_id) {
        auto manifest_it = project.manifests.find(tu_path_id);
        if(manifest_it == project.manifests.end()) {
            return;
        }
        for(auto& [fv, hash]: manifest_it->second.contributions) {
            if(project.file_versions.find(fv)->second.path_id == global_id) {
                manifest.contributions.emplace_back(fv, hash);
                return;
            }
        }
    };

    std::size_t hits = 0;
    std::size_t appended = 0;
    std::size_t rebuilt = 0;
    auto lookup_symbol = [&](index::SymbolHash hash) {
        return view.find_symbol(hash);
    };
    // Staged, not committed: a section that fails to decode rejects the
    // whole result mid-loop, and shards installed before that point would
    // leave the surviving manifest referencing variants the new blobs no
    // longer store.
    llvm::SmallVector<std::pair<std::uint32_t, index::Shard>> replacements;
    // (TU-local path id, rows hash) per serving section; the FileVersions
    // these will reference are interned only at commit.
    llvm::SmallVector<std::pair<std::uint32_t, std::uint64_t>> section_contributions;
    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        auto local_id = view.section_path(section);
        auto rows_hash = view.section_rows_hash(section);
        auto global_id = file_ids_map[local_id];
        auto consumed = view.path_hash(local_id);
        bool is_main = local_id == main_local_id;

        auto shard_it = workspace.shards.find(global_id);
        auto* shard = shard_it != workspace.shards.end() ? &shard_it->second : nullptr;

        // Fast path: this content generation's blob already stores these
        // rows — recording the contribution is the only work, no IO at all.
        if(consumed != 0 && shard && shard->loaded() && shard->content_hash() == consumed &&
           shard->has_variant(rows_hash)) {
            section_contributions.emplace_back(local_id, rows_hash);
            hits += 1;
            continue;
        }

        // Read and arbitrate the content the blob will pair the rows with.
        std::string header_content;
        llvm::StringRef content;
        if(is_main) {
            content = (*main_buf)->getBuffer();
        } else {
            auto path = workspace.path_pool.resolve(global_id);
            auto buf = llvm::MemoryBuffer::getFile(path);
            if(buf) {
                header_content = (*buf)->getBuffer().str();
                content = header_content;
            }
            // Unconditional, unlike the main-file read: an unreadable or
            // truncated-to-empty header must not slip past the arbitration
            // and pair the rows with content they were not built from. A
            // failed read is checked on its own — with no worker hash the
            // arbitration would otherwise wave the empty content through.
            if(!buf || !content_matches(local_id, content)) {
                LOG_INFO("Skip merge for {}: disk moved on since it was indexed", path);
                carry_old_contribution(global_id);
                continue;
            }
        }
        auto generation = consumed != 0 ? consumed : llvm::xxh3_64bits(content);

        // The same hit as the fast path above, verifiable for a hash-less
        // section only now that the disk read pinned the generation:
        // re-appending an already-stored variant would duplicate it in the
        // blob's variant table (write_shard asserts against exactly that).
        if(consumed == 0 && shard && shard->loaded() && shard->content_hash() == generation &&
           shard->has_variant(rows_hash)) {
            section_contributions.emplace_back(local_id, rows_hash);
            hits += 1;
            continue;
        }

        // The recomputed hash guards the variant identity alongside the
        // structural decode: rows installed under a hash they do not
        // reproduce would satisfy every later hit-path check for that hash
        // while the shard stores different rows.
        auto rows = view.decode_section_rows(section);
        if(!rows || rows->rows_hash() != rows_hash) {
            // Not a carry-and-skip like the moved-on case above: that one
            // self-corrects because the skipped file's recorded version is
            // stale by hash. A decode failure leaves every recorded version
            // matching the disk, so an installed manifest would be judged
            // fresh forever with this file's rows missing or stale — even
            // across restarts. Reject the whole result like the main-file
            // gate does; nothing is committed yet.
            LOG_WARN("Reject merge for {}: rows section for {} failed verification",
                     main_tu_path,
                     workspace.path_pool.resolve(global_id));
            return;
        }

        index::VariantInput fresh{rows_hash, &*rows, lookup_symbol};
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        if(shard && shard->loaded() && shard->content_hash() == generation) {
            // Same generation, new variant: merge it in, keeping every
            // stored variant — dead ones stay masked until the next save
            // compacts them.
            write_shard(*shard, shard->variants(), fresh, content, generation, os);
            appended += 1;
        } else {
            // New content generation (or no blob at all): rows from other
            // generations must never share offset storage with these, so
            // the blob starts over. Stale contributions from other TUs
            // simply stop matching any stored variant.
            write_shard(index::Shard(), {}, fresh, content, generation, os);
            rebuilt += 1;
        }

        auto replacement = index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
        if(!replacement.loaded()) {
            if(consumed == 0) {
                // Unverified pairing: the arbitration above cannot see the
                // disk shrinking under rows built from longer content, and
                // the blob's own range bounds catch it here instead. A
                // carry self-corrects — the recorded version is stale by
                // hash.
                LOG_INFO("Skip merge for {}: disk moved on since it was indexed",
                         workspace.path_pool.resolve(global_id));
                carry_old_contribution(global_id);
                continue;
            }
            // Hash-verified content always fits rows built from it: this
            // blob failed on the rows themselves (inverted or out-of-range
            // spans that kept wire structure and hash). Carrying would
            // install a manifest whose versions all match the disk, pinning
            // the stale rows as fresh forever — reject like the decode
            // failure above.
            LOG_WARN("Reject merge for {}: rows for {} do not form a valid shard",
                     main_tu_path,
                     workspace.path_pool.resolve(global_id));
            return;
        }
        replacements.emplace_back(global_id, std::move(replacement));
        section_contributions.emplace_back(local_id, rows_hash);
    }

    // The last gate and the first commit. A malformed reference bitmap (or
    // an out-of-range reference id) rejects the whole result for the same
    // reason a rows section that fails decode does above: everything the
    // merge would install reads as fresh forever, with the lost bits never
    // rebuilt.
    if(!project.merge(view, file_ids_map)) {
        LOG_WARN("Reject merge for {}: symbol reference bitmap failed verification", main_tu_path);
        return;
    }

    // Intern a FileVersion per file of the parse. The freshness baseline is
    // two-part and lives on the version, shared by every TU that consumed
    // it: the consumed-content hash from the compiler's own buffers, and a
    // stat fast path recorded only for files that provably did not change
    // since before the build started — for the rest the stat could describe
    // content the rows were never built from, so they re-earn their fast
    // path through a hash check instead (see file_version_stale).
    auto baseline_before_ns = fs::stat_baseline_before_ns(view.built_at());
    llvm::SmallVector<std::uint32_t> fv_of;
    fv_of.resize_for_overwrite(view.path_count());
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        llvm::StringRef path = view.path(i);
        auto hash = view.path_hash(i);

        fs::file_status status;
        bool stat_ok = !fs::status(path, status);
        bool untouched = stat_ok && fs::mtime_ns(status) <= baseline_before_ns;
        if(hash == 0 && untouched) {
            // The worker had no buffer to hash (e.g. behind a PCM); the
            // unchanged mtime proves the disk still holds the consumed
            // bytes, so hash it here.
            hash = hash_file(path);
        }

        auto fv = project.intern_file_version(file_ids_map[i], hash);
        if(untouched) {
            auto& record = project.file_versions.find(fv)->second;
            record.size = status.getSize();
            record.mtime_ns = fs::mtime_ns(status);
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
    dirty_manifests.insert(tu_path_id);
    global_dirty = true;

    LOG_INFO(
        "Merged TUIndex: {} paths, {} sections ({} hits, {} appended, {} rebuilt), "
        "{} merged_shards",
        view.path_count(),
        view.section_count(),
        hits,
        appended,
        rebuilt,
        workspace.shards.size());
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
        write_shard(shard, live, {}, shard.content(), shard.content_hash(), os);
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

    // The global blob goes last: a crash mid-batch then strands only
    // manifests, which load() drops by their generation stamp — the
    // reverse order would strand a global claiming symbols in files whose
    // rows never landed.
    if(global_dirty) {
        std::string bytes;
        llvm::raw_string_ostream os(bytes);
        project.serialize_global(os, workspace.path_pool);
        batch.push_back({index::IndexBlobKind::Global, "global", std::move(bytes)});
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
        } else {
            global_dirty = true;
        }
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

void Indexer::load() {
    if(!workspace.index_storage)
        return;
    auto& storage = *workspace.index_storage;
    auto& project = workspace.project_index;
    ScopedTimer timer;

    auto sweep_all = [&] {
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
            return;
        }
        // No global table means no resolvable manifests: everything else
        // is unreachable data, swept so it cannot survive as orphans.
        sweep_all();
        return;
    }
    llvm::DenseMap<std::uint32_t, std::uint64_t> manifest_pins;
    if(!project.load_global(global->getBuffer(), workspace.path_pool, manifest_pins)) {
        LOG_INFO("Discarding old-format index global blob");
        sweep_all();
        storage.remove(index::IndexBlobKind::Global, "global");
        return;
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
    for(auto& key: dead_manifests) {
        storage.remove(index::IndexBlobKind::Manifest, key);
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
            storage.remove(index::IndexBlobKind::Manifest,
                           blob_key(workspace.path_pool.resolve(tu)));
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
    llvm::SmallVector<std::string> orphans;
    storage.for_each_key(index::IndexBlobKind::Shard, [&](llvm::StringRef key) {
        if(!expected_keys.contains(key)) {
            orphans.push_back(key.str());
        }
    });
    for(auto& key: orphans) {
        storage.remove(index::IndexBlobKind::Shard, key);
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
    if(contexts.resolve_command(file_path, params.directory, params.arguments, nullptr) ==
       CommandSource::Fallback)
        co_return;

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
        merge(result.value().tu_index_data.data(), result.value().tu_index_data.size());
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
    } else if(result.has_value() && result.value().tu_index_data.empty()) {
        LOG_WARN("[{}/{}] Index returned empty TUIndex for {}", index, total, file_path);
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
