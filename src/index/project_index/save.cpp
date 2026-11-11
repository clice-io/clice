module clice;

import :index.database;
import :index.project_index;
import :index.project_index.manifest;
import :index.project_index.records;
import :index.search;
import :index.shard;
import :index.shard.instantiations;
import :support.logging;
import :support.text;

namespace clice::index {

namespace {

template <typename Records>
std::uint32_t stored_count(const Records& records) {
    return static_cast<std::uint32_t>(
        ranges::count_if(records, [](const auto& entry) { return entry.second.has_value(); }));
}

}  // namespace

void ProjectIndex::begin_save(SaveBatch& batch, IndexReport& report) {
    // Reset up front: a save that commits nothing must not keep exposing
    // the previous pass's count as current.
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
    //
    // LIMITATION: all of it runs on the caller's loop — compacting, copying
    // each changed shard's bytes into the batch, encoding the changed
    // records — and only the write is the thread pool's: a checkpoint of a
    // full index of a project of LLVM's size holds the loop for most of a
    // second. Sharing the shards' bytes with the batch and encoding over a
    // snapshot off the loop would end it.
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
            replace_shard(file, {});
            report.add_index_changed(file);
            requeue_owners(file, report);
            continue;
        }
        if(!current->has_dead_variants()) {
            continue;
        }
        auto bytes = merge_shards(*current, live, {});
        auto replacement =
            Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(as_chars(bytes)));
        assert(replacement.loaded() && "a freshly written shard blob must verify");
        replace_shard(file, std::move(replacement));
    }

    // A file's instantiations live with its shard of the same generation.
    // Dead uses and the resolutions only they held compact like dead
    // variants, and the keys of runs that are no longer their unit's current
    // one leave with them; a file left with neither is retired.
    //
    // LIMITATION: a key that resolves nothing leaves with the unit that
    // walked it though other units skipped it as known, which no use
    // records: the next unit instantiating it walks it once more.
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
            replace_instantiations(file, {});
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
            replace_instantiations(file, {});
            report.add_index_changed(file);
            continue;
        }
        auto replacement =
            InstantiationShard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(as_chars(*bytes)));
        assert(replacement.loaded() && "a freshly written instantiation blob must verify");
        replace_instantiations(file, std::move(replacement));
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

    // The header names its files by path ids, handed out before the
    // records are taken. Without a search index the pending symbols mean
    // nothing past this session, every search scanning the records then;
    // the pending files' names of internal linkage are in no record.
    header.search_pending.clear();
    if(search.index.loaded()) {
        header.search_pending.assign(search.pending_symbols.begin(), search.pending_symbols.end());
        ranges::sort(header.search_pending);
    }
    header.search_pending_files.clear();
    for(auto file: search.pending_files) {
        header.search_pending_files.push_back(path_id(file));
    }
    ranges::sort(header.search_pending_files);
    header.owed_units.clear();
    for(auto unit: owed) {
        header.owed_units.push_back(path_id(unit));
    }
    ranges::sort(header.owed_units);

    auto& blobs = batch.blobs;
    batch.shards =
        stored_count(shard_records.changed) + stored_count(instantiation_records.changed);
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
    auto stored_bytes = [](const auto& value) {
        return copy_bytes(value.bytes());
    };
    take(manifest_records, [&](const TUManifest& manifest) { return encode_manifest(manifest); });
    take(shard_records, stored_bytes);
    take(instantiation_records, stored_bytes);
    take(file_records, encode_value);
    take(dependent_records, encode_value);
    take(internal_records, encode_value);
    take(symbol_records, encode_value);
    take(local_records, encode_value);
    take(path_records, [](const std::string& name) { return copy_bytes(name); });
    take(version_records, encode_value);

    for(auto& segment: search.writes) {
        blobs.push_back(std::move(segment));
    }
    search.writes.clear();
    for(auto id: search.drops) {
        batch.removals.push_back({BlobKind::SearchSegment, number_key(id)});
    }
    search.drops.clear();

    if(!blobs.empty() || !batch.removals.empty() || header_changed) {
        blobs.push_back({BlobKind::Header, "header", encode_record(header)});
    }
    header_changed = false;
}

void ProjectIndex::abandon_save(SaveBatch& batch) {
    // A merge across the await may have changed a record the batch carried:
    // what it did stands.
    auto restore = [&]<typename Value>(Records<Value>& records) {
        for(auto& entry: records.written) {
            records.changed.try_emplace(entry.getKey(), std::move(entry.second));
        }
        records.written.clear();
    };
    restore(manifest_records);
    restore(shard_records);
    restore(instantiation_records);
    restore(file_records);
    restore(dependent_records);
    restore(internal_records);
    restore(symbol_records);
    restore(local_records);
    restore(path_records);
    restore(version_records);
    header_changed = true;
    for(auto& blob: batch.blobs) {
        if(blob.kind == BlobKind::SearchSegment) {
            search.writes.push_back(std::move(blob));
        }
    }
    for(auto& removal: batch.removals) {
        if(removal.kind == BlobKind::SearchSegment) {
            search.drops.push_back(*key_number<std::uint32_t>(removal.key));
        }
    }
}

async::task<bool> ProjectIndex::migrate_views(BlobDatabase& db,
                                              async::event_loop& loop,
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
    drop_written(manifest_records);
    drop_written(file_records);
    drop_written(dependent_records);
    drop_written(internal_records);
    drop_written(symbol_records);
    drop_written(local_records);
    drop_written(path_records);
    drop_written(version_records);
    // The shards the batch wrote stay resident: they own their bytes until
    // the rebind below moves them onto the snapshot, as the ones read
    // before.
    saved_shards =
        stored_count(shard_records.written) + stored_count(instantiation_records.written);
    auto keep_written = [&]<typename Value>(Records<Value>& records) {
        for(auto& entry: records.written) {
            records.cached[entry.getKey()] = std::move(entry.second);
        }
        records.written.clear();
    };
    keep_written(shard_records);
    keep_written(instantiation_records);
    co_return co_await rebind_views(db, loop, grew);
}

async::task<bool> ProjectIndex::rebind_views(BlobDatabase& db,
                                             async::event_loop& loop,
                                             bool at_once) {
    // The shards read from the database borrow the previous snapshot, or own
    // the bytes the batch just wrote: both move onto the new snapshot, whose
    // bytes are identical — the writer lock keeps every key the batch left
    // alone byte-identical. Both snapshots stay valid across the yields, so
    // queries between batches may observe a mix of old and new pointers
    // with identical meaning.
    constexpr std::uint32_t rebind_batch = 512;
    auto rebind = [&]<typename Value>(Records<Value>& records) -> async::task<> {
        std::vector<std::string> resident;
        for(auto& entry: records.cached) {
            if(entry.second) {
                resident.push_back(entry.getKey().str());
            }
        }
        for(std::uint32_t i = 0; i < resident.size() && !db.corrupted(); i += 1) {
            if(!at_once && i != 0 && i % rebind_batch == 0) {
                co_await async::yield(loop);
            }
            // A save between the batches may have replaced or retired it.
            auto it = records.cached.find(resident[i]);
            if(it == records.cached.end() || !it->second) {
                continue;
            }
            auto blob = db.read(records.kind, resident[i]);
            if(!blob || !it->second->rebind(std::move(blob.buffer))) {
                LOG_ERROR(
                    "The index database's {} record {} does not match the one it "
                    "saved; the database is discarded and the project indexed again",
                    records.kind,
                    llvm::toHex(resident[i]));
                damage = true;
                break;
            }
        }
    };
    co_await rebind(shard_records);
    co_await rebind(instantiation_records);
    // The search index borrows the snapshot, but for the segments no batch
    // committed yet, which own their bytes.
    if(search.index.loaded()) {
        for(std::uint32_t i = 0; i < header.search_segments.size() && !damage; i += 1) {
            auto key = number_key(header.search_segments[i]);
            if(ranges::any_of(search.writes,
                              [&](const BlobDatabase::Blob& blob) { return blob.key == key; })) {
                continue;
            }
            auto blob = db.read(BlobKind::SearchSegment, key);
            if(!blob || !search.index.rebind(i, std::move(blob.buffer))) {
                LOG_ERROR("The index database lost a search segment its header lists");
                damage = true;
            }
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
