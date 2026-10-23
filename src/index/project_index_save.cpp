module;

#include "support/logging.macros.h"
#include "support/prelude.h"

module clice;

import :index.database;
import :index.instantiation_shard;
import :index.project_index;
import :index.project_records;
import :index.search_index;
import :index.shard;
import :index.tu_manifest;
import :support.bytes;
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
    // entries may misdescribe the symbol records until then.
    if(search.damaged()) {
        return true;
    }
    if(rebuild == SearchRebuild::Behind) {
        return false;
    }
    // A missing one — never built, or persisted under another pin than
    // the header's — leaves searches scanning every symbol record until
    // the first idle save.
    if(!search.loaded()) {
        return merges_since_search_build > 0 || symbol_count() > 0;
    }
    // A twentieth of the units, so a small project refreshes on any
    // merge and a large one every twenty at most.
    auto units = (db ? db->count(BlobKind::TUManifest) : 0) +
                 static_cast<std::uint32_t>(changed_manifests.size());
    return merges_since_search_build > std::min<std::uint32_t>(20, units / 20);
}

Internals ProjectIndex::internal_names(Fid file) const {
    Internals names;
    auto* current = shard(file);
    if(!current) {
        return names;
    }
    current->for_each_local_symbol([&](SymbolHash hash, const SymbolView& symbol) {
        if(symbol.scope == SymbolScope::Internal && is_searchable_kind(symbol.kind) &&
           !symbol.name.empty() && current->declares(hash)) {
            names.push_back({
                .hash = hash,
                .name = symbol.name.str(),
                .args = symbol.args.str(),
                .parent = symbol.parent,
                .kind = symbol.kind,
                .flags = symbol.flags,
            });
        }
        return true;
    });
    ranges::sort(names, [](const InternalName& lhs, const InternalName& rhs) {
        return lhs.hash < rhs.hash;
    });
    return names;
}

kota::task<> ProjectIndex::rebuild_search(SearchRebuild rebuild) {
    if(!search_rebuild_due(rebuild)) {
        co_return;
    }
    ScopedTimer timer;
    SearchSnapshot snapshot;
    // A rebuild changes the header (below): the save writes it with the
    // search blob it pins.
    snapshot.generation = header.search_generation + 1;
    llvm::DenseMap<std::uint32_t, std::uint32_t> path_index;
    auto path_of = [&](Fid file) {
        auto [it, inserted] =
            path_index.try_emplace(file.raw, static_cast<std::uint32_t>(snapshot.paths.size()));
        if(inserted) {
            snapshot.paths.push_back(portable(files.resolve(file)));
        }
        return it->second;
    };
    for_each_symbol([&](SymbolHash hash, const SymbolView& symbol, std::uint32_t references) {
        // Unnamed scopes stay in: the build names their members through
        // them, and leaves them out of the index itself.
        if(!is_searchable_kind(symbol.kind) || symbol.name.empty()) {
            return true;
        }
        snapshot.entries.push_back({
            .hash = hash,
            .name = symbol.name.str(),
            .args = symbol.args.str(),
            .parent = symbol.parent,
            .kind = symbol.kind,
            .flags = symbol.flags,
            .file = symbol.file != no_file ? path_of(Fid{symbol.file}) : no_file,
            .reference_files = references,
        });
        return true;
    });
    // The symbols of internal linkage are in no symbol record but the
    // internals records of the files declaring them; those of the files
    // merged since the last save are only in their shards yet.
    auto add_internals = [&](Fid file, const Internals& names) {
        for(auto& name: names) {
            snapshot.entries.push_back({
                .hash = name.hash,
                .name = name.name,
                .args = name.args,
                .parent = name.parent,
                .kind = name.kind,
                .flags = name.flags,
                .file = path_of(file),
                .local = true,
            });
        }
    };
    scan(internal_records, decode_internals, [&](llvm::StringRef key, const Internals& names) {
        auto* record = read(file_records, key, decode_file);
        auto file = record ? fid_of(record->path) : Fid{};
        if(file.valid() && !internals_changed.contains(file)) {
            add_internals(file, names);
        }
        return true;
    });
    for(auto file: internals_changed) {
        add_internals(file, internal_names(file));
    }
    auto merges_in_snapshot = merges_since_search_build;
    // The entries pending now are in the snapshot; the ones a merge changes
    // during the build stay pending once it is adopted.
    llvm::SmallVector<SymbolHash> pending_in_snapshot(pending_symbols.begin(),
                                                      pending_symbols.end());
    llvm::SmallVector<Fid> pending_files_in_snapshot(pending_files.begin(), pending_files.end());
    changed_during_search_build.emplace();
    auto finished = llvm::make_scope_exit([&] { changed_during_search_build.reset(); });

    std::vector<std::byte> bytes;
    co_await kota::queue([&] { bytes = build_search_blob(snapshot); });
    SearchIndex built;
    if(!built.load(llvm::MemoryBuffer::getMemBufferCopy(as_chars(bytes)))) {
        LOG_ERROR("The rebuilt search index does not load; keeping the previous one");
        co_return;
    }
    search = std::move(built);
    header.search_generation = snapshot.generation;
    header_changed = true;
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

    // Compact the shards whose variant set shrank: queries already mask the
    // dead variants, this erases them for real before the blob reaches disk.
    // A file with no live variant left in its blob is retired entirely:
    // either no contribution remains, or the remaining ones pin hashes a
    // newer content generation replaced (their TUs have not reindexed yet)
    // — compacting to those pins would write a blob with no variants at
    // all, while retiring serves the same nothing the mask already does.
    // The pinning TUs are re-enqueued: no in-process event would rebuild
    // their variants otherwise (a reverted file even reads fresh by hash).
    llvm::SmallVector<Fid> marked(compact.begin(), compact.end());
    ranges::sort(marked);
    compact.clear();
    for(auto file: marked) {
        auto* current = shard(file);
        if(!current) {
            continue;
        }
        auto live = live_variants(file);
        if(ranges::none_of(live, [&](std::uint64_t hash) { return current->has_variant(hash); })) {
            // Held empty: the database keeps the blob until the batch
            // commits, and it must not be read back meanwhile.
            shards[file] = Shard();
            changed_shards.erase(file);
            retired_shards.insert(file);
            auto& record = change_file(file);
            record.generation = 0;
            record.stored.clear();
            report.add_index_changed(file);
            requeue_owners(file, report);
            search_file_changed(file);
            continue;
        }
        if(!current->has_dead_variants()) {
            continue;
        }
        auto bytes = merge_shards(*current, live, {});
        auto replacement =
            Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(as_chars(bytes)));
        assert(replacement.loaded() && "a freshly written shard blob must verify");
        change_file(file).stored = replacement.variants();
        shards[file] = std::move(replacement);
        changed_shards.insert(file);
    }

    // A file's instantiations live with its shard of the same generation.
    // Dead uses and the resolutions only they held compact like dead
    // variants, and the keys of runs that are no longer their unit's current
    // one leave with them; a file left with neither is retired.
    auto is_current = [&](const Producer& run) {
        return current_run(run);
    };
    for(auto file: marked) {
        auto* held = instantiation_shard(file);
        if(!held) {
            continue;
        }
        auto* current = shard(file);
        if(!current || current->content_hash() != held->content_hash()) {
            retire_instantiations(file);
            report.add_index_changed(file);
            continue;
        }
        bool gone_runs = false;
        held->for_each_key([&](const InstantiationShard::Key& key) {
            gone_runs = !is_current(*key.producer);
            return !gone_runs;
        });
        if(!held->has_dead_uses() && !gone_runs) {
            continue;
        }
        auto bytes =
            merge_instantiations(*held, live_uses(file), is_current, nullptr, {}, std::nullopt);
        if(!bytes) {
            retire_instantiations(file);
            report.add_index_changed(file);
            continue;
        }
        auto replacement =
            InstantiationShard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(as_chars(*bytes)));
        assert(replacement.loaded() && "a freshly written instantiation blob must verify");
        instantiations[file] = std::move(replacement);
        changed_instantiations.insert(file);
    }

    // The names of internal linkage the compacted shards declare, for the
    // search index's next build.
    for(auto file: internals_changed) {
        auto key = file_key(file);
        auto names = internal_names(file);
        if(names.empty() && !read(internal_records, key, decode_internals)) {
            continue;
        }
        internal_records.changed[key] =
            names.empty() ? std::nullopt : std::optional(std::move(names));
    }
    internals_changed.clear();

    // Everything below hands out path ids before the records are taken:
    // the manifests name their files by them, and so does the header.
    auto& blobs = batch.blobs;
    for(auto& [tu, manifest]: changed_manifests) {
        auto key = file_key(tu);
        if(!manifest) {
            batch.removals.push_back({BlobKind::TUManifest, std::move(key)});
            continue;
        }
        blobs.push_back({BlobKind::TUManifest, std::move(key), encode_manifest(*manifest)});
    }
    header.search_pending.assign(pending_symbols.begin(), pending_symbols.end());
    ranges::sort(header.search_pending);
    header.search_pending_files.clear();
    for(auto file: pending_files) {
        header.search_pending_files.push_back(path_id(file));
    }
    ranges::sort(header.search_pending_files);
    header.owed_units.clear();
    for(auto unit: owed) {
        header.owed_units.push_back(path_id(unit));
    }
    ranges::sort(header.owed_units);

    for(auto file: changed_shards) {
        blobs.push_back(
            {BlobKind::Shard, file_key(file), copy_bytes(shards.find(file)->second.bytes())});
    }
    for(auto file: changed_instantiations) {
        blobs.push_back({BlobKind::InstantiationShard,
                         file_key(file),
                         copy_bytes(instantiations.find(file)->second.bytes())});
    }
    for(auto file: retired_shards) {
        batch.removals.push_back({BlobKind::Shard, file_key(file)});
    }
    for(auto file: retired_instantiations) {
        batch.removals.push_back({BlobKind::InstantiationShard, file_key(file)});
    }
    batch.shards = changed_shards.size() + changed_instantiations.size();

    auto take = [&]<typename Value>(Records<Value>& records, auto&& encode_value) {
        for(auto& entry: records.changed) {
            auto key = entry.getKey().str();
            if(entry.second) {
                blobs.push_back({records.kind, key, encode_value(*entry.second)});
            } else {
                batch.removals.push_back({records.kind, key});
            }
            records.cached.erase(entry.getKey());
            records.written[entry.getKey()] = std::move(entry.second);
        }
        records.changed.clear();
    };
    auto encode_value = [](const auto& value) {
        return encode_record(value);
    };
    take(file_records, encode_value);
    take(dependent_records, encode_value);
    take(internal_records, encode_value);
    take(symbol_records, encode_value);
    take(shared_local_records, encode_value);
    take(path_records, [](const std::string& name) { return copy_bytes(name); });

    if(!blobs.empty() || !batch.removals.empty() || header_changed || !search_bytes.empty()) {
        blobs.push_back({BlobKind::Header, "header", encode_record(header)});
    }
    header_changed = false;
    writing_search = !search_bytes.empty();
    if(writing_search) {
        blobs.push_back({BlobKind::SearchIndex, "search", std::move(search_bytes)});
        search_bytes.clear();
    }

    for(auto& [tu, manifest]: changed_manifests) {
        written_manifests[tu] = std::move(manifest);
    }
    changed_manifests.clear();
    written_shards = std::move(changed_shards);
    changed_shards.clear();
    written_instantiations = std::move(changed_instantiations);
    changed_instantiations.clear();
    written_retired_shards = std::move(retired_shards);
    retired_shards.clear();
    written_retired_instantiations = std::move(retired_instantiations);
    retired_instantiations.clear();
    writing_shards = batch.shards;
}

void ProjectIndex::abandon_save(SaveBatch& batch) {
    auto restore = [&]<typename Value>(Records<Value>& records) {
        for(auto& entry: records.written) {
            records.changed.try_emplace(entry.getKey(), std::move(entry.second));
        }
        records.written.clear();
    };
    restore(file_records);
    restore(dependent_records);
    restore(internal_records);
    restore(symbol_records);
    restore(shared_local_records);
    restore(path_records);
    for(auto& [tu, manifest]: written_manifests) {
        changed_manifests.try_emplace(tu, std::move(manifest));
    }
    written_manifests.clear();
    // A merge across the await may have replaced or retired a written
    // shard: what it did stands.
    for(auto file: written_shards) {
        if(auto it = shards.find(file); it != shards.end() && it->second.loaded()) {
            changed_shards.insert(file);
        }
    }
    for(auto file: written_instantiations) {
        if(auto it = instantiations.find(file); it != instantiations.end() && it->second.loaded()) {
            changed_instantiations.insert(file);
        }
    }
    for(auto file: written_retired_shards) {
        if(!changed_shards.contains(file)) {
            retired_shards.insert(file);
        }
    }
    for(auto file: written_retired_instantiations) {
        if(!changed_instantiations.contains(file)) {
            retired_instantiations.insert(file);
        }
    }
    written_shards.clear();
    written_instantiations.clear();
    written_retired_shards.clear();
    written_retired_instantiations.clear();
    header_changed = true;
    if(writing_search) {
        search_bytes = std::move(ranges::find_if(batch.blobs, [](const BlobDatabase::Blob& blob) {
                                     return blob.kind == BlobKind::SearchIndex;
                                 })->bytes);
        writing_search = false;
    }
    writing_shards = 0;
}

kota::task<bool> ProjectIndex::migrate_views(BlobDatabase& db,
                                             kota::event_loop& loop,
                                             IndexReport& report) {
    // The records are read from the database the batch went to: the one
    // replacing a discarded database is bound at its first save.
    this->db = &db;
    // A full-map write committed nothing (abandon_save restored it); growing
    // retires every snapshot at once, so the rebind below runs to completion
    // before the first yield.
    bool grew = false;
    switch(db.grow()) {
        case BlobDatabase::Growth::None: break;
        case BlobDatabase::Growth::Grown:
            grew = true;
            // The refused batch is changed again; the grown map takes it now.
            report.owes_retry = true;
            break;
        case BlobDatabase::Growth::Refused: co_return !db.corrupted();
    }
    if(!grew) {
        // The committed changes stay in memory, shadowing the snapshot,
        // until a newer one opens: it may not (every reader slot taken), and
        // dropping them before would read the old records back.
        if(auto advanced = db.advance_read_snapshot(); !advanced) {
            LOG_WARN("Index read-snapshot advance failed: {}", advanced.error());
            co_return !db.corrupted();
        }
    }
    auto drop_written = [&]<typename Value>(Records<Value>& records) {
        records.written.clear();
        records.cached.clear();
    };
    drop_written(file_records);
    drop_written(dependent_records);
    drop_written(internal_records);
    drop_written(symbol_records);
    drop_written(shared_local_records);
    drop_written(path_records);
    written_manifests.clear();
    written_retired_shards.clear();
    written_retired_instantiations.clear();
    saved_shards = writing_shards;
    writing_shards = 0;
    writing_search = false;

    // The resident shards borrow the previous snapshot, or own the bytes
    // the batch just wrote: both move onto the new snapshot, whose bytes are
    // identical — the writer lock keeps every key the batch left alone
    // byte-identical. Both snapshots stay valid across the yields, so
    // queries between batches may observe a mix of old and new pointers
    // with identical meaning.
    constexpr std::uint32_t rebind_batch = 512;
    auto rebind =
        [&](auto& held, const llvm::DenseSet<Fid>& changed, BlobKind kind) -> kota::task<> {
        llvm::SmallVector<Fid> resident;
        for(auto& [file, value]: held) {
            if(value.loaded() && !changed.contains(file)) {
                resident.push_back(file);
            }
        }
        for(std::uint32_t i = 0; i < resident.size() && !db.corrupted(); i += 1) {
            if(!grew && i != 0 && i % rebind_batch == 0) {
                co_await kota::yield(loop);
            }
            // A merge between the batches may have replaced or retired it.
            auto it = held.find(resident[i]);
            if(it == held.end() || !it->second.loaded() || changed.contains(resident[i])) {
                continue;
            }
            auto blob = db.read(kind, file_key(resident[i]));
            if(!blob || !it->second.rebind(std::move(blob.buffer))) {
                LOG_ERROR("Index blob for {} diverged during snapshot migration",
                          files.resolve(resident[i]));
                damage = true;
                break;
            }
        }
    };
    co_await rebind(shards, changed_shards, BlobKind::Shard);
    co_await rebind(instantiations, changed_instantiations, BlobKind::InstantiationShard);
    written_shards.clear();
    written_instantiations.clear();
    // The search index borrows the snapshot, unless it is a build no batch
    // committed yet, which owns its bytes.
    if(search.loaded() && search_bytes.empty()) {
        auto blob = db.read(BlobKind::SearchIndex, "search");
        if(!blob || !search.load(std::move(blob.buffer))) {
            LOG_ERROR("The index database lost the search index its header pins");
            damage = true;
        }
    }
    // A view left unrebound — a shard that diverged and the ones after it,
    // a lost search index — still borrows the old snapshot: it stays, and
    // the caller discards the index before anything reads them.
    if(db.corrupted() || damage) {
        co_return false;
    }
    db.retire_old_snapshot();
    co_return true;
}

}  // namespace clice::index
