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

bool ProjectIndex::search_update_due(SearchRebuild rebuild) const {
    if(search.loaded() && !search.damaged()) {
        if(!pending_symbols.empty() || !pending_files.empty()) {
            return true;
        }
        std::uint32_t newer = 0;
        for(std::uint32_t run = 1; run < search.run_count(); run += 1) {
            newer += search.run_size(run);
        }
        return rebuild == SearchRebuild::Idle && newer * 2 >= search.run_size(0) && newer > 0;
    }
    // A missing or damaged index is built from the records; until then
    // searches scan every symbol record.
    switch(rebuild) {
        case SearchRebuild::Never: return false;
        case SearchRebuild::Behind: return search.damaged() || pending_symbols.size() > 10000;
        case SearchRebuild::Idle:
            return search.damaged() || symbol_count() > 0 || !pending_symbols.empty() ||
                   !pending_files.empty();
    }
    std::unreachable();
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

SearchSnapshot ProjectIndex::full_search_snapshot() {
    SearchSnapshot snapshot;
    scan(symbol_records, decode_symbol, [&](llvm::StringRef key, const Symbol& symbol) {
        auto hash = key_number<SymbolHash>(key);
        if(!hash || reserved_key(*hash)) {
            damaged_record(BlobKind::Symbol, key);
            return true;
        }
        // Unnamed scopes stay in: the build names their members through
        // them, and leaves them out of the index itself.
        if(!is_searchable_kind(symbol.kind) || symbol.name.empty()) {
            return true;
        }
        snapshot.entries.push_back({
            .hash = *hash,
            .name = symbol.name,
            .args = symbol.args,
            .parent = symbol.parent,
            .kind = symbol.kind,
            .flags = symbol.flags,
            .file = symbol.file,
            .reference_files = static_cast<std::uint32_t>(symbol.reference_files.cardinality()),
        });
        return true;
    });
    // The symbols of internal linkage are in no symbol record but the
    // internals records of the files declaring them; those of the files
    // merged since the last save are only in their shards yet.
    auto add_internals = [&](std::uint32_t path, const Internals& names) {
        for(auto& name: names) {
            snapshot.entries.push_back({
                .hash = name.hash,
                .name = name.name,
                .args = name.args,
                .parent = name.parent,
                .kind = name.kind,
                .flags = name.flags,
                .file = path,
                .local = true,
            });
        }
    };
    scan(internal_records, decode_internals, [&](llvm::StringRef key, const Internals& names) {
        auto* record = read(file_records, key, decode_file);
        if(record && !internals_changed.contains(fid_of(record->path))) {
            add_internals(record->path, names);
        }
        return true;
    });
    for(auto file: internals_changed) {
        add_internals(path_id(file), internal_names(file));
    }
    return snapshot;
}

SearchSnapshot ProjectIndex::search_snapshot(llvm::ArrayRef<SymbolHash> symbols,
                                             llvm::ArrayRef<Fid> files) {
    SearchSnapshot snapshot;
    auto entry_of = [](SymbolHash hash, const Symbol& symbol) {
        return SearchEntry{
            .hash = hash,
            .name = symbol.name,
            .args = symbol.args,
            .parent = symbol.parent,
            .kind = symbol.kind,
            .flags = symbol.flags,
            .file = symbol.file,
            .reference_files = static_cast<std::uint32_t>(symbol.reference_files.cardinality()),
        };
    };
    for(auto hash: symbols) {
        if(auto* found = symbol(hash)) {
            snapshot.entries.push_back(entry_of(hash, *found));
        }
    }
    for(auto file: files) {
        auto path = path_id(file);
        snapshot.covered.push_back(path);
        auto add = [&](const Internals& names) {
            for(auto& name: names) {
                snapshot.entries.push_back({
                    .hash = name.hash,
                    .name = name.name,
                    .args = name.args,
                    .parent = name.parent,
                    .kind = name.kind,
                    .flags = name.flags,
                    .file = path,
                    .local = true,
                });
            }
        };
        if(internals_changed.contains(file)) {
            add(internal_names(file));
        } else if(auto* names = read(internal_records, file_key(file), decode_internals)) {
            add(*names);
        }
    }
    ranges::sort(snapshot.covered);
    // The scopes above the entries, as far as the records name them: a
    // scope of internal linkage another file declares is not looked up,
    // and ends the chain there.
    llvm::DenseSet<SymbolHash> known;
    for(auto& entry: snapshot.entries) {
        known.insert(entry.hash);
    }
    auto climb = [&](SymbolHash parent) {
        while(parent != 0 && known.insert(parent).second) {
            auto* found = symbol(parent);
            if(!found) {
                return;
            }
            snapshot.scopes.push_back(entry_of(parent, *found));
            parent = found->parent;
        }
    };
    for(std::size_t i = 0; i < snapshot.entries.size(); i += 1) {
        climb(snapshot.entries[i].parent);
    }
    return snapshot;
}

void ProjectIndex::drop_search_runs(std::uint32_t runs) {
    auto& ends = header.search_run_ends;
    auto kept = static_cast<std::uint32_t>(ends.size()) - runs;
    auto first = kept == 0 ? 0 : ends[kept - 1];
    for(auto id: llvm::ArrayRef(header.search_segments).drop_front(first)) {
        auto key = number_key(id);
        // A segment no batch committed yet is only in memory.
        auto it = ranges::find_if(search_writes,
                                  [&](const BlobDatabase::Blob& blob) { return blob.key == key; });
        if(it != search_writes.end()) {
            search_writes.erase(it);
        } else {
            search_drops.push_back(id);
        }
    }
    header.search_segments.resize(first);
    ends.resize(kept);
    header_changed = true;
}

kota::task<> ProjectIndex::update_search(SearchRebuild rebuild) {
    if(!search_update_due(rebuild)) {
        co_return;
    }
    ScopedTimer timer;
    auto runs = search.run_count();
    bool full = !search.loaded() || search.damaged();
    std::uint32_t folded = 0;
    SearchSnapshot snapshot;
    if(!full) {
        llvm::SetVector<SymbolHash> symbols(pending_symbols.begin(), pending_symbols.end());
        llvm::SetVector<Fid> files(pending_files.begin(), pending_files.end());
        snapshot = search_snapshot(symbols.getArrayRef(), files.getArrayRef());
        // The newest runs fold into the one built now while it comes to half
        // of the next older one at least, so run sizes halve from the first
        // on and a symbol is read again a logarithmic number of times; the
        // first run folds only into a full build, once the runs after it come
        // to half of it.
        if(rebuild != SearchRebuild::Never) {
            auto size = static_cast<std::uint32_t>(
                snapshot.covered.size() + ranges::count_if(snapshot.entries, [](auto& entry) {
                    return searchable(entry.kind, entry.name, entry.flags);
                }));
            auto newer = size;
            for(std::uint32_t run = 1; run < runs; run += 1) {
                newer += search.run_size(run);
            }
            while(folded + 1 < runs && size * 2 >= search.run_size(runs - 1 - folded)) {
                size += search.run_size(runs - 1 - folded);
                folded += 1;
            }
            full = rebuild == SearchRebuild::Idle && newer * 2 >= search.run_size(0);
        }
        if(!full && folded > 0) {
            for(auto run = runs - folded; run < runs; run += 1) {
                search.for_each_source(
                    run,
                    [&](SymbolHash hash) { symbols.insert(hash); },
                    [&](std::uint32_t path) {
                        if(auto file = fid_of(path); file.valid()) {
                            files.insert(file);
                        }
                    });
            }
            snapshot = search_snapshot(symbols.getArrayRef(), files.getArrayRef());
        }
    }
    if(full) {
        snapshot = full_search_snapshot();
    }
    auto read_ms = timer.ms();
    // The entries pending now are in the snapshot; the ones a merge changes
    // during the build stay pending once it is adopted.
    llvm::SmallVector<SymbolHash> pending_in_snapshot(pending_symbols.begin(),
                                                      pending_symbols.end());
    llvm::SmallVector<Fid> pending_files_in_snapshot(pending_files.begin(), pending_files.end());
    changed_during_search_build.emplace();
    auto finished = llvm::make_scope_exit([&] { changed_during_search_build.reset(); });

    std::vector<std::vector<std::byte>> segments;
    co_await kota::queue([&] { segments = build_search_segments(snapshot); });
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> buffers;
    for(auto& segment: segments) {
        buffers.push_back(llvm::MemoryBuffer::getMemBufferCopy(as_chars(segment)));
    }
    auto replaced = full ? search.run_count() : folded;
    drop_search_runs(full ? static_cast<std::uint32_t>(header.search_run_ends.size()) : folded);
    if(!search.replace(replaced, std::move(buffers))) {
        LOG_ERROR(
            "The built search index does not load; searches scan the records until a "
            "full build");
        search.clear();
        drop_search_runs(static_cast<std::uint32_t>(header.search_run_ends.size()));
        co_return;
    }
    std::uint64_t bytes = 0;
    for(auto& segment: segments) {
        bytes += segment.size();
        header.search_segments.push_back(header.next_segment);
        search_writes.push_back(
            {BlobKind::SearchSegment, number_key(header.next_segment), std::move(segment)});
        header.next_segment += 1;
    }
    header.search_run_ends.push_back(static_cast<std::uint32_t>(header.search_segments.size()));
    for(auto hash: pending_in_snapshot) {
        if(!changed_during_search_build->symbols.contains(hash)) {
            pending_symbols.erase(hash);
        }
    }
    for(auto file: pending_files_in_snapshot) {
        if(!changed_during_search_build->files.contains(file)) {
            pending_files.erase(file);
        }
    }
    LOG_PERF("index",
             "phase=search_build full={} folded={} entries={} segments={} bytes={} read_ms={} "
             "elapsed_ms={}",
             full,
             folded,
             snapshot.entries.size(),
             segments.size(),
             bytes,
             read_ms,
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

    auto& blobs = batch.blobs;
    for(auto& [tu, manifest]: changed_manifests) {
        auto key = file_key(tu);
        if(!manifest) {
            batch.removals.push_back({BlobKind::TUManifest, std::move(key)});
            continue;
        }
        blobs.push_back({BlobKind::TUManifest, std::move(key), encode_manifest(*manifest)});
    }
    // The header names its files by path ids, handed out before the
    // records are taken. Without a search index the pending symbols mean
    // nothing past this session, every search scanning the records then;
    // the pending files' names of internal linkage are in no record.
    header.search_pending.clear();
    if(search.loaded()) {
        header.search_pending.assign(pending_symbols.begin(), pending_symbols.end());
        ranges::sort(header.search_pending);
    }
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
    take(version_records, encode_value);

    for(auto& segment: search_writes) {
        blobs.push_back(std::move(segment));
    }
    search_writes.clear();
    for(auto id: search_drops) {
        batch.removals.push_back({BlobKind::SearchSegment, number_key(id)});
    }
    search_drops.clear();

    if(!blobs.empty() || !batch.removals.empty() || header_changed) {
        blobs.push_back({BlobKind::Header, "header", encode_record(header)});
    }
    header_changed = false;

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
    restore(version_records);
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
    for(auto& blob: batch.blobs) {
        if(blob.kind == BlobKind::SearchSegment) {
            search_writes.push_back(std::move(blob));
        }
    }
    for(auto& removal: batch.removals) {
        if(removal.kind == BlobKind::SearchSegment) {
            search_drops.push_back(*key_number<std::uint32_t>(removal.key));
        }
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
    drop_written(version_records);
    written_manifests.clear();
    written_retired_shards.clear();
    written_retired_instantiations.clear();
    saved_shards = writing_shards;
    writing_shards = 0;

    written_shards.clear();
    written_instantiations.clear();
    co_return co_await rebind_views(db, loop, grew);
}

kota::task<bool> ProjectIndex::rebind_views(BlobDatabase& db,
                                            kota::event_loop& loop,
                                            bool at_once) {
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
            if(!at_once && i != 0 && i % rebind_batch == 0) {
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
    // The search index borrows the snapshot, but for the segments no batch
    // committed yet, which own their bytes.
    if(search.loaded()) {
        for(std::uint32_t i = 0; i < header.search_segments.size() && !damage; i += 1) {
            auto key = number_key(header.search_segments[i]);
            if(ranges::any_of(search_writes,
                              [&](const BlobDatabase::Blob& blob) { return blob.key == key; })) {
                continue;
            }
            auto blob = db.read(BlobKind::SearchSegment, key);
            if(!blob || !search.rebind(i, std::move(blob.buffer))) {
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
