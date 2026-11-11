module clice;

import :index.database;
import :index.project_index;
import :index.project_index.records;
import :index.search;
import :index.shard;
import :support.logging;
import :support.text;

namespace clice::index {

namespace {

/// A symbol record as the search index takes it.
SearchEntry search_entry(SymbolHash hash, const Symbol& symbol) {
    SearchEntry entry{
        .hash = hash,
        .name = symbol.name,
        .args = symbol.args,
        .parent = symbol.parent,
        .kind = symbol.kind,
        .flags = symbol.view().flags,
        .reference_files = static_cast<std::uint32_t>(symbol.reference_files.cardinality()),
    };
    for(auto path: symbol.declaration_files) {
        entry.files.push_back(path);
    }
    return entry;
}

/// A name of internal linkage the shard of the file `path` declares.
SearchEntry search_entry(std::uint32_t path, const InternalName& name) {
    return {
        .hash = name.hash,
        .name = name.name,
        .args = name.args,
        .parent = name.parent,
        .kind = name.kind,
        .flags = name.flags,
        .files = {path},
        .local = true,
    };
}

}  // namespace

void ProjectIndex::search_file_changed(Fid file) {
    search.pending_files.insert(file);
    internals_changed.insert(file);
    if(search.changed_during_build) {
        search.changed_during_build->files.insert(file);
    }
}

bool ProjectIndex::search_update_due(SearchRebuild rebuild) const {
    if(search.index.loaded() && !search.index.damaged()) {
        if(!search.pending_symbols.empty() || !search.pending_files.empty()) {
            return true;
        }
        std::uint32_t newer = 0;
        for(std::uint32_t run = 1; run < search.index.run_count(); run += 1) {
            newer += search.index.run_size(run);
        }
        return rebuild == SearchRebuild::Idle && newer * 2 >= search.index.run_size(0) && newer > 0;
    }
    // A missing or damaged index is built from the records; until then
    // searches scan every symbol record.
    switch(rebuild) {
        case SearchRebuild::Never: return false;
        case SearchRebuild::Behind:
            return search.index.damaged() || search.pending_symbols.size() > 10000;
        case SearchRebuild::Idle:
            return search.index.damaged() || symbol_count() > 0 ||
                   !search.pending_symbols.empty() || !search.pending_files.empty();
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
        snapshot.entries.push_back(search_entry(*hash, symbol));
        return true;
    });
    // The symbols of internal linkage are in no symbol record but the
    // internals records of the files declaring them; those of the files
    // merged since the last save are only in their shards yet.
    auto add_internals = [&](std::uint32_t path, const Internals& names) {
        for(auto& name: names) {
            snapshot.entries.push_back(search_entry(path, name));
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
    for(auto hash: symbols) {
        if(auto* found = symbol(hash)) {
            snapshot.entries.push_back(search_entry(hash, *found));
        }
    }
    for(auto file: files) {
        auto path = path_id(file);
        snapshot.covered.push_back(path);
        auto add = [&](const Internals& names) {
            for(auto& name: names) {
                snapshot.entries.push_back(search_entry(path, name));
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
            snapshot.scopes.push_back(search_entry(parent, *found));
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
        auto it = ranges::find_if(search.writes,
                                  [&](const BlobDatabase::Blob& blob) { return blob.key == key; });
        if(it != search.writes.end()) {
            search.writes.erase(it);
        } else {
            search.drops.push_back(id);
        }
    }
    header.search_segments.resize(first);
    ends.resize(kept);
    header_changed = true;
}

async::task<> ProjectIndex::update_search(SearchRebuild rebuild) {
    if(!search_update_due(rebuild)) {
        co_return;
    }
    Timer timer;
    auto runs = search.index.run_count();
    bool full = !search.index.loaded() || search.index.damaged();
    std::uint32_t folded = 0;
    SearchSnapshot snapshot;
    if(!full) {
        llvm::SetVector<SymbolHash> symbols(search.pending_symbols.begin(),
                                            search.pending_symbols.end());
        llvm::SetVector<Fid> files(search.pending_files.begin(), search.pending_files.end());
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
                newer += search.index.run_size(run);
            }
            while(folded + 1 < runs && size * 2 >= search.index.run_size(runs - 1 - folded)) {
                size += search.index.run_size(runs - 1 - folded);
                folded += 1;
            }
            full = rebuild == SearchRebuild::Idle && newer * 2 >= search.index.run_size(0);
        }
        if(!full && folded > 0) {
            for(auto run = runs - folded; run < runs; run += 1) {
                search.index.for_each_source(
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
    llvm::SmallVector<SymbolHash> pending_in_snapshot(search.pending_symbols.begin(),
                                                      search.pending_symbols.end());
    llvm::SmallVector<Fid> pending_files_in_snapshot(search.pending_files.begin(),
                                                     search.pending_files.end());
    search.changed_during_build.emplace();
    auto finished = llvm::make_scope_exit([&] { search.changed_during_build.reset(); });

    std::vector<std::vector<std::byte>> segments;
    co_await async::queue([&] { segments = build_search_segments(snapshot); });
    std::vector<std::unique_ptr<llvm::MemoryBuffer>> buffers;
    for(auto& segment: segments) {
        buffers.push_back(llvm::MemoryBuffer::getMemBufferCopy(as_chars(segment)));
    }
    auto replaced = full ? search.index.run_count() : folded;
    drop_search_runs(full ? static_cast<std::uint32_t>(header.search_run_ends.size()) : folded);
    if(!search.index.replace(replaced, std::move(buffers))) {
        LOG_ANOMALY(IndexReadBack,
                    "The built search index does not load; searches scan the records until a "
                    "full build");
        search.index.clear();
        drop_search_runs(static_cast<std::uint32_t>(header.search_run_ends.size()));
        co_return;
    }
    std::uint64_t bytes = 0;
    for(auto& segment: segments) {
        bytes += segment.size();
        header.search_segments.push_back(header.next_segment);
        search.writes.push_back(
            {BlobKind::SearchSegment, number_key(header.next_segment), std::move(segment)});
        header.next_segment += 1;
    }
    header.search_run_ends.push_back(static_cast<std::uint32_t>(header.search_segments.size()));
    for(auto hash: pending_in_snapshot) {
        if(!search.changed_during_build->symbols.contains(hash)) {
            search.pending_symbols.erase(hash);
        }
    }
    for(auto file: pending_files_in_snapshot) {
        if(!search.changed_during_build->files.contains(file)) {
            search.pending_files.erase(file);
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

}  // namespace clice::index
