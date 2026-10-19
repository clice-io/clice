module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.database;
import :index.instantiation_shard;
import :index.project_index;
import :index.search_index;
import :index.shard;
import :index.tu_manifest;
import :support.logging;
import :support.timer;

namespace clice::index {

bool ProjectIndex::search_rebuild_due(SearchRebuild rebuild) const {
    if(rebuild == SearchRebuild::Never) {
        return false;
    }
    auto built = search.size();
    if(pending_symbols.size() > std::max<std::uint32_t>(10000, built / 20)) {
        return true;
    }
    // A damaged index is replaced at the first save, idle or not: its
    // entries may misdescribe the table until then.
    if(search.damaged()) {
        return true;
    }
    if(rebuild == SearchRebuild::Behind) {
        return false;
    }
    // A missing one — never built, or persisted under another pin than
    // the table's — leaves searches scanning the whole table until the
    // first idle save.
    if(!search.loaded()) {
        return symbol_count() > 0;
    }
    // A twentieth of the units, so a small project refreshes on any
    // merge and a large one every twenty at most.
    return merges_since_search_build > std::min<std::uint32_t>(20, unit_count() / 20);
}

kota::task<> ProjectIndex::rebuild_search(SearchRebuild rebuild) {
    if(!search_rebuild_due(rebuild)) {
        co_return;
    }
    ScopedTimer timer;
    SearchSnapshot snapshot;
    snapshot.entries.reserve(symbol_count());
    // A rebuild dirties the table (below): the save writes the project table
    // under its next generation, and the search blob is pinned to it.
    snapshot.generation = generation + 1;
    llvm::DenseMap<std::uint32_t, std::uint32_t> path_index;
    for_each_symbol([&](SymbolHash hash, const SymbolView& symbol, std::uint32_t references) {
        // Unnamed scopes stay in: the build names their members through
        // them, and leaves them out of the index itself.
        if(!is_searchable_kind(symbol.kind) || symbol.name.empty()) {
            return true;
        }
        auto file = no_file;
        if(symbol.file != no_file) {
            auto [it, inserted] =
                path_index.try_emplace(symbol.file,
                                       static_cast<std::uint32_t>(snapshot.paths.size()));
            if(inserted) {
                snapshot.paths.push_back(portable(files.resolve(Fid{symbol.file})));
            }
            file = it->second;
        }
        snapshot.entries.push_back({
            .hash = hash,
            .name = symbol.name.str(),
            .args = symbol.args.str(),
            .parent = symbol.parent,
            .kind = symbol.kind,
            .flags = symbol.flags,
            .file = file,
            .reference_files = references,
        });
        return true;
    });
    // The symbols of internal linkage are in no table but the shards of the
    // files declaring them: the previous build's carry over for the files
    // whose names did not change since, and the shards of the rest are read.
    auto add_local = [&](Fid file, SearchEntry entry) {
        auto [path, inserted] =
            path_index.try_emplace(file.raw, static_cast<std::uint32_t>(snapshot.paths.size()));
        if(inserted) {
            snapshot.paths.push_back(portable(files.resolve(file)));
        }
        entry.file = path->second;
        snapshot.entries.push_back(std::move(entry));
    };
    llvm::DenseSet<Fid> reread;
    if(search.loaded()) {
        reread.insert(pending_files.begin(), pending_files.end());
        search.for_each_local([&](llvm::StringRef path, const SearchEntry& entry) {
            auto file = files.intern(local(path));
            if(!reread.contains(file)) {
                add_local(file, entry);
            }
        });
    } else {
        for_each_indexed_file([&](Fid file) { reread.insert(file); });
    }
    for(auto file: reread) {
        auto* shard = this->shard(file);
        if(!shard) {
            continue;
        }
        shard->for_each_local_symbol([&](SymbolHash hash, const SymbolView& symbol) {
            if(symbol.scope != SymbolScope::Internal || !is_searchable_kind(symbol.kind) ||
               symbol.name.empty() || !shard->declares(hash)) {
                return true;
            }
            add_local(file,
                      {
                          .hash = hash,
                          .name = symbol.name.str(),
                          .args = symbol.args.str(),
                          .parent = symbol.parent,
                          .kind = symbol.kind,
                          .flags = symbol.flags,
                          .local = true,
                      });
            return true;
        });
    }
    auto merges_in_snapshot = merges_since_search_build;
    // The entries pending now are in the snapshot; the ones a merge changes
    // during the build stay pending once it is adopted.
    llvm::SmallVector<SymbolHash> pending_in_snapshot(pending_symbols.begin(),
                                                      pending_symbols.end());
    llvm::SmallVector<Fid> pending_files_in_snapshot(pending_files.begin(), pending_files.end());
    changed_during_search_build.emplace();
    auto finished = llvm::make_scope_exit([&] { changed_during_search_build.reset(); });

    std::string bytes;
    co_await kota::queue([&] { bytes = build_search_blob(snapshot); });
    SearchIndex built;
    if(!built.load(llvm::MemoryBuffer::getMemBufferCopy(bytes))) {
        LOG_ERROR("The rebuilt search index does not load; keeping the previous one");
        co_return;
    }
    search = std::move(built);
    // The table pins the blob it was saved with: a reader adopts the
    // persisted search index only under this generation.
    search_generation = snapshot.generation;
    table_dirty = true;
    merges_since_search_build -= merges_in_snapshot;
    for(auto hash: pending_in_snapshot) {
        if(search.contains(hash) && !changed_during_search_build->symbols.contains(hash)) {
            pending_symbols.erase(hash);
        }
    }
    for(auto file: pending_files_in_snapshot) {
        if(!changed_during_search_build->files.contains(file)) {
            pending_files.erase(file);
        }
    }
    search_bytes = std::move(bytes);
    LOG_PERF("index",
             "phase=search_build symbols={} bytes={} elapsed_ms={}",
             search.size(),
             search_bytes.size(),
             timer.ms());
}

void ProjectIndex::begin_save(SaveBatch& batch, IndexReport& report) {
    // Reset up front: a save that commits nothing must not keep exposing
    // the previous round's count as current.
    saved_shards = 0;

    // Compact shards whose variant set shrank: queries already mask the
    // dead variants, this erases them for real before the blob reaches disk.
    // A file with no live variant left in its blob is retired entirely
    // below: either no contribution remains, or the remaining ones pin
    // hashes a newer content generation replaced (their TUs have not
    // reindexed yet) — compacting to those pins would write a blob with no
    // variants at all, while retiring serves the same nothing the mask
    // already does. The pinning TUs are re-enqueued: no in-process event
    // would rebuild their variants otherwise (a reverted file even reads fresh
    // by hash), only a read after a restart finding the variants lost.
    // The candidates are the shards read so far and the files a TU's
    // removal touched, read now: a blob nothing reads was compacted when it
    // was written.
    llvm::DenseSet<Fid> shrunk_files = std::move(shrunk);
    shrunk.clear();
    llvm::SmallVector<Fid> candidates(shrunk_files.begin(), shrunk_files.end());
    for(auto& [path_id, shard]: shards) {
        if(shard.loaded() && !shrunk_files.contains(path_id)) {
            candidates.push_back(path_id);
        }
    }
    llvm::SmallVector<Fid> retired;
    for(auto path_id: candidates) {
        auto live = live_variants(path_id);
        if(live.empty()) {
            retired.push_back(path_id);
            continue;
        }
        // A blob found lost here heals through take_lost.
        if(!this->shard(path_id)) {
            continue;
        }
        auto& shard = shards.find(path_id)->second;
        if(llvm::none_of(live, [&](std::uint64_t hash) { return shard.has_variant(hash); })) {
            retired.push_back(path_id);
            continue;
        }
        if(!shard.has_dead_variants()) {
            continue;
        }
        auto bytes = merge_shards(shard, live, {});
        auto replacement = Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(bytes));
        assert(replacement.loaded() && "a freshly written shard blob must verify");
        shard = std::move(replacement);
        dirty_shards.insert(path_id);
    }
    for(auto path_id: retired) {
        shards.erase(path_id);
        dirty_shards.erase(path_id);
        report.add_index_changed(path_id);
        requeue_owners(path_id, report);
    }

    // A file's instantiations live with its shard of the same generation.
    // Dead uses and the resolutions only they held compact like dead
    // variants, and the keys of runs that are no longer their unit's current
    // one leave with them; a file left with neither is retired.
    auto is_current = [&](const Producer& run) {
        return current_run(run);
    };
    llvm::SmallVector<Fid> instantiation_candidates;
    for(auto path_id: shrunk_files) {
        if(this->instantiation_shard(path_id)) {
            instantiation_candidates.push_back(path_id);
        }
    }
    for(auto& [path_id, held]: instantiations) {
        if(held.loaded() && !shrunk_files.contains(path_id)) {
            instantiation_candidates.push_back(path_id);
        }
    }
    llvm::SmallVector<Fid> retired_files;
    for(auto path_id: instantiation_candidates) {
        auto& held = instantiations.find(path_id)->second;
        if(shard_generation(path_id) != held.content_hash()) {
            retired_files.push_back(path_id);
            continue;
        }
        bool gone_runs = false;
        held.for_each_key([&](const InstantiationShard::Key& key) {
            gone_runs = !is_current(*key.producer);
            return !gone_runs;
        });
        if(!held.has_dead_uses() && !gone_runs) {
            continue;
        }
        auto bytes =
            merge_instantiations(held, live_uses(path_id), is_current, nullptr, {}, std::nullopt);
        if(!bytes) {
            retired_files.push_back(path_id);
            continue;
        }
        auto replacement =
            InstantiationShard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(*bytes));
        assert(replacement.loaded() && "a freshly written instantiation blob must verify");
        held = std::move(replacement);
        dirty_instantiations.insert(path_id);
    }
    for(auto path_id: retired_files) {
        retire_instantiations(path_id);
        report.add_index_changed(path_id);
    }

    // Move the dirty state into the batch: everything below serializes
    // from copies, so merges committing across the write await simply
    // re-dirty for the next save. The id vectors parallel the batch so a
    // failed entry can be re-dirtied by its slot.
    auto& blobs = batch.blobs;
    batch.removals = std::move(pending_removals);
    pending_removals.clear();
    for(auto path_id: retired) {
        batch.removals.push_back({BlobKind::Shard, key_of(path_id)});
    }
    batch.retired_ids.assign(retired_instantiations.begin(), retired_instantiations.end());
    retired_instantiations.clear();
    for(auto path_id: batch.retired_ids) {
        batch.removals.push_back({BlobKind::InstantiationShard, key_of(path_id)});
    }
    for(auto path_id: dirty_shards) {
        auto it = shards.find(path_id);
        assert(it != shards.end() && "dirty shards stay resident until retirement");
        blobs.push_back({BlobKind::Shard, key_of(path_id), it->second.bytes().str()});
        batch.shard_ids.push_back(path_id);
    }
    for(auto path_id: dirty_instantiations) {
        auto it = instantiations.find(path_id);
        assert(it != instantiations.end() &&
               "dirty instantiation shards stay resident until retirement");
        blobs.push_back({BlobKind::InstantiationShard, key_of(path_id), it->second.bytes().str()});
        batch.instantiation_ids.push_back(path_id);
    }

    // One generation per persisted project table, recorded in every
    // manifest of the batch and pinned per TU inside the project table
    // (serialize_table): load() adopts a manifest only at its pinned
    // generation. Without the pin an ordering check alone cannot tell a
    // deliberately unchanged older manifest from one whose update failed
    // while the project table committed — both FileVersion sets can stay fully
    // resolvable (a reindex that changed entries or the include tree only).
    if(table_dirty) {
        generation += 1;
    }
    for(auto tu_path_id: dirty_manifests) {
        auto it = manifests.find(tu_path_id);
        auto key = key_of(tu_path_id);
        // Dirty with no in-memory manifest means dropped (drop): the
        // persisted blob must go too, or a restart resurrects the TU's
        // index as fresh.
        if(it == manifests.end()) {
            batch.removals.push_back({BlobKind::TUManifest, std::move(key)});
            continue;
        }
        it->second.table_generation = generation;
        blobs.push_back({BlobKind::TUManifest,
                         std::move(key),
                         serialize_manifest(export_manifest(it->second))});
        batch.manifest_ids.push_back(tu_path_id);
    }

    // The project table follows the shards and manifests: a crash mid-batch
    // then strands only manifests, which load() drops by their generation
    // pin — the reverse order would strand a project table reporting symbols in
    // files whose shards never committed.
    batch.had_table = table_dirty;
    if(table_dirty) {
        batch.table_slot = blobs.size();
        blobs.push_back({BlobKind::ProjectTable, "global", serialize_table()});
    }
    // The search blob moves into the batch and back out if the write
    // never commits: it is derived state, so nothing else would re-create
    // it before the next rebuild.
    if(!search_bytes.empty()) {
        batch.search_slot = blobs.size();
        blobs.push_back({BlobKind::SearchIndex, "search", std::move(search_bytes)});
        search_bytes.clear();
    }
    batch.index_slots = blobs.size();

    dirty_shards.clear();
    dirty_instantiations.clear();
    dirty_manifests.clear();
    table_dirty = false;

    // A deferred load-time removal can name a key this very save re-writes:
    // the removed TU was re-enqueued at load and has already re-indexed.
    // Puts run before removes inside write(), so the stale removal would
    // delete the fresh blob — the put wins.
    if(!batch.removals.empty()) {
        llvm::DenseSet<std::pair<std::uint8_t, llvm::StringRef>> putting;
        for(auto& put: blobs) {
            putting.insert({std::to_underlying(put.kind), llvm::StringRef(put.key)});
        }
        llvm::erase_if(batch.removals, [&](const BlobKey& remove) {
            return putting.contains({std::to_underlying(remove.kind), llvm::StringRef(remove.key)});
        });
    }

    // The dirty set was moved into the batch so merges committing across the
    // write await re-dirty for the next save; the in-flight count keeps
    // pending_shard_writes() truthful meanwhile — a stats reader polling for
    // "shard writes finished" must not observe zero while the commit is still
    // running (and saved_shards still holds its reset).
    saving_shards = batch.shard_ids.size() + batch.instantiation_ids.size();
}

void ProjectIndex::abandon_save(SaveBatch& batch) {
    // Unioned with whatever re-dirtied across the await. A merge across
    // the await may retire a file's instantiations: retired, they stay so.
    redirty(batch);
    for(auto path_id: batch.retired_ids) {
        if(!instantiations.contains(path_id)) {
            retired_instantiations.insert(path_id);
        }
    }
    dirty_manifests.insert(batch.manifest_ids.begin(), batch.manifest_ids.end());
    table_dirty = table_dirty || batch.had_table;
    restore_unwritten();
    if(batch.search_slot) {
        search_bytes = std::move(batch.blobs[*batch.search_slot].bytes);
    }
    pending_removals.append(std::make_move_iterator(batch.removals.begin()),
                            std::make_move_iterator(batch.removals.end()));
    saving_shards = 0;
}

void ProjectIndex::finish_save(SaveBatch& batch, llvm::ArrayRef<std::uint32_t> failed) {
    // An entry the database failed to commit is re-dirtied so a later save
    // retries it; discarded, the cache would trail the in-memory index
    // until an unrelated merge happens to dirty the same entry or a
    // restart rebuilds it. (Failed removals need no retry: load() drops
    // stale manifests by their generation pin and removes orphan shards.)
    auto shard_count = batch.shard_ids.size();
    auto instantiation_count = batch.instantiation_ids.size();
    auto manifest_count = batch.manifest_ids.size();
    std::uint32_t failed_shards = 0;
    for(auto i: failed) {
        if(i < shard_count) {
            failed_shards += 1;
            dirty_shards.insert(batch.shard_ids[i]);
        } else if(i - shard_count < instantiation_count) {
            failed_shards += 1;
            auto path_id = batch.instantiation_ids[i - shard_count];
            if(instantiations.contains(path_id)) {
                dirty_instantiations.insert(path_id);
            }
        } else if(i - shard_count - instantiation_count < manifest_count) {
            dirty_manifests.insert(batch.manifest_ids[i - shard_count - instantiation_count]);
        } else if(batch.table_slot && i == *batch.table_slot) {
            table_dirty = true;
            batch.table_slot.reset();
        } else if(batch.search_slot && i == *batch.search_slot) {
            search_bytes = std::move(batch.blobs[i].bytes);
        }
    }
    // The entries and the units the committed blob holds now read from it;
    // a failed project table write keeps them aside for the retry.
    if(batch.table_slot) {
        rebase(llvm::MemoryBuffer::getMemBufferCopy(batch.blobs[*batch.table_slot].bytes));
    } else if(batch.had_table) {
        restore_unwritten();
    }
    saved_shards = shard_count + instantiation_count - failed_shards;
    saving_shards = 0;
}

void ProjectIndex::redirty(const SaveBatch& batch) {
    dirty_shards.insert(batch.shard_ids.begin(), batch.shard_ids.end());
    for(auto path_id: batch.instantiation_ids) {
        if(instantiations.contains(path_id)) {
            dirty_instantiations.insert(path_id);
        }
    }
}

kota::task<bool> ProjectIndex::migrate_views(BlobDatabase& db,
                                             kota::event_loop& loop,
                                             IndexReport& report) {
    // A full-map write left nothing committed (everything is dirty again);
    // growing retires every snapshot at once, so the rebind below must run
    // to completion before the first yield — and the table, which every
    // read reaches, reads a copy of its own across it.
    if(base && db.growth_pending()) {
        base->rebind(llvm::MemoryBuffer::getMemBufferCopy(base->buffer->getBuffer()));
    }
    auto grown = db.grow();
    if(!grown) {
        // Degenerate (address space exhausted): borrowed views may already
        // be dead, so every one is dropped rather than left dangling. Their
        // persisted bytes stay intact for the next read to fetch again.
        LOG_ERROR("Index database growth failed: {}", grown.error());
        if(db.corrupted()) {
            co_return false;
        }
        drop_borrowed_views();
        co_return true;
    }
    bool grew = *grown;
    if(grew) {
        // The refused batch is dirty again; the grown map takes it now.
        report.owes_retry = true;
    } else {
        // The LMDB backend hands out pointers into its resident read
        // snapshot; after a commit the resident shards migrate onto a
        // fresh snapshot so the old one can be retired. Both snapshots
        // stay valid across the yields and the blobs are byte-identical,
        // so queries between batches may observe a mix of old and new
        // pointers with identical meaning. Shards whose write just failed
        // are dirty again by now (their bytes never committed) and stay
        // memory-backed until a later save.
        if(auto advanced = db.advance_read_snapshot(); !advanced) {
            LOG_WARN("Index read-snapshot advance failed: {}", advanced.error());
            co_return !db.corrupted();
        }
    }
    // The table moves first, onto the bytes the save wrote or the ones it
    // read; a copy left by a save or a growth goes with it.
    if(base) {
        auto blob = db.read(BlobKind::ProjectTable, "global");
        if(!blob || !base->rebind(std::move(blob.buffer))) {
            if(db.corrupted()) {
                co_return false;
            }
            // Unreachable under the writer lock; a copy outlives the
            // snapshot the table borrows.
            LOG_ERROR("Index project table diverged during snapshot migration");
            assert(false && "the persisted project table must survive snapshot migration");
            base->rebind(llvm::MemoryBuffer::getMemBufferCopy(base->buffer->getBuffer()));
        }
    }

    constexpr std::uint32_t rebind_batch = 512;
    llvm::SmallVector<Fid> resident;
    for(auto& [path_id, shard]: shards) {
        if(shard.loaded() && !dirty_shards.contains(path_id)) {
            resident.push_back(path_id);
        }
    }
    for(std::uint32_t i = 0; i < resident.size(); i += 1) {
        if(!grew && i != 0 && i % rebind_batch == 0) {
            co_await kota::yield(loop);
        }
        auto path_id = resident[i];
        auto it = shards.find(path_id);
        if(it == shards.end() || dirty_shards.contains(path_id)) {
            continue;
        }
        auto blob = db.read(BlobKind::Shard, key_of(path_id));
        if(!blob || !it->second.rebind(std::move(blob.buffer))) {
            // Corruption can also show up first here (a damaged page only
            // this re-read reaches); the recovery sheds the whole resident
            // set, nothing per-shard to do.
            if(db.corrupted()) {
                break;
            }
            // Unreachable under the writer lock; the shard is dropped and
            // its owners requeued to rebuild the shard, while keeping the
            // old view would dangle once the snapshot retires.
            LOG_ERROR("Index shard for {} diverged during snapshot migration",
                      files.resolve(path_id));
            assert(false && "persisted shard must survive snapshot migration");
            shards.erase(path_id);
            report.add_index_changed(path_id);
            requeue_owners(path_id, report);
        }
    }
    llvm::SmallVector<Fid> resident_instantiations;
    for(auto& [path_id, held]: instantiations) {
        if(held.loaded() && !dirty_instantiations.contains(path_id)) {
            resident_instantiations.push_back(path_id);
        }
    }
    for(std::uint32_t i = 0; i < resident_instantiations.size() && !db.corrupted(); i += 1) {
        if(!grew && i != 0 && i % rebind_batch == 0) {
            co_await kota::yield(loop);
        }
        auto path_id = resident_instantiations[i];
        auto it = instantiations.find(path_id);
        if(it == instantiations.end() || dirty_instantiations.contains(path_id)) {
            continue;
        }
        auto blob = db.read(BlobKind::InstantiationShard, key_of(path_id));
        if(!blob || !it->second.rebind(std::move(blob.buffer))) {
            if(db.corrupted()) {
                break;
            }
            LOG_ERROR("Index instantiations for {} diverged during snapshot migration",
                      files.resolve(path_id));
            assert(false && "persisted instantiations must survive snapshot migration");
            instantiations.erase(path_id);
            report.add_index_changed(path_id);
            requeue_owners(path_id, report);
        }
    }
    // Corruption observed by any read since the write-time check — this
    // loop's, or a query's during its yields — condemns the database; the
    // recovery sheds every borrowed view before the environment closes.
    if(db.corrupted()) {
        co_return false;
    }
    db.retire_old_snapshot();
    co_return true;
}

llvm::SmallVector<Fid> ProjectIndex::drop_borrowed_views() {
    llvm::SmallVector<Fid> dropped;
    llvm::SmallVector<Fid> shed;
    for(auto& [path_id, shard]: shards) {
        if(!dirty_shards.contains(path_id)) {
            shed.push_back(path_id);
        }
    }
    for(auto path_id: shed) {
        if(shards.find(path_id)->second.loaded()) {
            dropped.push_back(path_id);
        }
        shards.erase(path_id);
    }
    shed.clear();
    for(auto& [path_id, held]: instantiations) {
        if(!dirty_instantiations.contains(path_id)) {
            shed.push_back(path_id);
        }
    }
    for(auto path_id: shed) {
        if(instantiations.find(path_id)->second.loaded()) {
            dropped.push_back(path_id);
        }
        instantiations.erase(path_id);
    }
    return dropped;
}

void ProjectIndex::recover_corrupt_database(IndexReport& report) {
    saved_shards = 0;
    // The table outlives the condemned database it borrows: the symbols it
    // holds serve until the units reindex.
    if(base) {
        base->rebind(llvm::MemoryBuffer::getMemBufferCopy(base->buffer->getBuffer()));
    }
    // The dropped views' bytes lived in the condemned database alone. An
    // instantiation shard's users are the TUs including the file, which
    // contribute to it.
    for(auto path_id: drop_borrowed_views()) {
        report.add_index_changed(path_id);
        requeue_owners(path_id, report);
    }
    // The manifests and blobs of the units the session did not merge lived
    // in the condemned database alone: those units leave the table and
    // reindex, and the shards held in memory stop serving their variants.
    if(base) {
        for(auto& unit: base->columns->units) {
            auto tu = base->remap[unit.file];
            if(base_unit(tu)) {
                hide_base_unit(tu);
                report.add_reindex(tu);
            }
        }
        for(auto& contributed: base->columns->contributions) {
            refresh_live(base->remap[contributed.file]);
        }
        for(auto& used: base->columns->uses) {
            refresh_live(base->remap[used.file]);
        }
    }
    stale_units.reset();
    db = nullptr;
    for(auto tu_path_id: llvm::make_first_range(manifests)) {
        dirty_manifests.insert(tu_path_id);
    }
    table_dirty = true;
}

}  // namespace clice::index
