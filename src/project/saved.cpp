module clice;

import :command.file_kind;
import :feature.diagnostics;
import :index.database;
import :index.format;
import :index.project_index;
import :project.background;
import :project.command;
import :project.discovery;
import :project.graph.pch;
import :project.graph.pcm;
import :project.project;
import :project.saved;
import :support.bytes;
import :support.environment;
import :support.json;
import :support.logging;
import :support.timer;
import :vfs.cache_store;
import :vfs.file_table;
import :vfs.path;

namespace clice {

namespace {

/// The precompiled records: a JSON document in the index database, the
/// persisted form of PCH/PCM validity metadata and self-containment
/// checks. Paths are persisted as spellings and re-interned at load —
/// runtime fids are not stable across sessions; deps persist as (spelling, content hash), so
/// the records reference no version ids and self-heal against any index
/// state.
struct SavedInput {
    /// index into the blob's paths table
    std::uint32_t path;

    std::uint64_t hash;

    bool missing;
};

struct SavedPCH {
    std::string key;

    /// CacheStore key in the "pch" namespace; absent in records written
    /// before pairs had names of their own.
    std::optional<std::string> blob;

    std::uint32_t bound;

    /// See PCHEntry::whole_file; absent from the records of a binary
    /// before it.
    KANNOTATE(&whole_file, presence = optional)
    bool whole_file = false;

    std::vector<SavedInput> deps;

    /// See PCHEntry::base; absent for none.
    std::optional<std::string> base;

    /// See PCHEntry::base_blob; absent for none, and in records written
    /// before it was recorded.
    std::optional<std::string> base_blob;
};

struct SavedPCM {
    std::string key;

    /// CacheStore key in the "pcm" namespace; absent in records written
    /// before PCMs had names of their own.
    std::optional<std::string> blob;

    std::uint32_t source_file;

    std::string module_name;

    std::vector<SavedInput> deps;
};

struct SavedSelfContainment {
    /// index into the blob's paths table
    std::uint32_t header;

    /// The command the header borrows (CommandTable::command_hash).
    std::uint64_t command;

    bool self_contained;

    /// The header's text the self-containment check compiled.
    std::uint64_t text;

    /// The header's disk content when the check ran.
    std::uint64_t disk;

    std::vector<SavedInput> deps;
};

struct Records {
    std::vector<std::string> paths;

    /// index_format_version the .pch.idx indexes were written with (one
    /// binary writes them all). A mismatch drops every PCH entry at load so
    /// the pairs rebuild immediately, instead of the mismatch showing up
    /// lazily on the first overlay query — which cannot trigger a rebuild.
    std::uint32_t pch_index_format = 0;

    std::vector<SavedPCH> pch;

    std::vector<SavedPCM> pcm;

    /// Absent from the records a binary older than the self-containment
    /// checks wrote.
    KANNOTATE(&trials, presence = optional)
    std::vector<SavedSelfContainment> trials;

    /// The missing_context_scoring the trials were judged under; absent
    /// from the records of a binary before it.
    KANNOTATE(&trials_scoring, presence = optional)
    std::uint32_t trials_scoring = 0;
};

}  // namespace

void SavedMembers::read(index::BlobDatabase& database) {
    auto blob = database.read(index::BlobKind::Members, "cdb");
    if(!blob) {
        return;
    }
    if(Blob read; json::from_string(std::string_view(blob.buffer->getBuffer()), read)) {
        read_blob = std::move(read);
        persisted_bytes = blob.buffer->getBuffer().str();
    } else {
        unparsed = true;
    }
}

llvm::SmallVector<Spelling> SavedMembers::databases(const index::ProjectIndex& index) const {
    llvm::SmallVector<Spelling> result;
    if(!read_blob) {
        return result;
    }
    for(auto& entry: read_blob->entries) {
        for(auto& source: entry.sources) {
            auto absolute = index.local(source);
            if(!ranges::contains(result, absolute)) {
                result.push_back(std::move(absolute));
            }
        }
    }
    return result;
}

std::vector<Spelling> SavedMembers::provisional(const index::ProjectIndex& index) const {
    std::vector<Spelling> result;
    if(read_blob) {
        for(auto& file: read_blob->provisional) {
            result.push_back(index.local(file));
        }
    }
    return result;
}

std::optional<std::string> SavedMembers::serialize(const Blob& live, std::uint64_t provisional) {
    serialized_provisional = provisional;
    auto json = json::to_string(live, path_json);
    if(!json) {
        LOG_WARN("Failed to serialize the build's members: {}", json.error().to_string());
    }
    if(!json || *json == persisted_bytes) {
        dirty = false;
        return std::nullopt;
    }
    return std::move(*json);
}

SavedMembers::Blob Project::recorded_members() {
    SavedMembers::Blob members;
    auto all = cdb.entries();
    for(std::size_t i = 0; i < all.size();) {
        auto path_id = all[i].file;
        while(i < all.size() && all[i].file == path_id) {
            i += 1;
        }
        // In build order, which registration order — the order databases
        // were discovered in — must not leak into. Entries only inactive
        // configurations declare are not compiled by this view.
        std::vector<std::string> sources;
        for(auto& candidate: entries(path_id)) {
            auto database = cdb.source_path(candidate.source);
            // Commands added by hand come from no database a later session
            // could miss.
            if(database.empty()) {
                continue;
            }
            auto source = index.portable(database);
            if(!ranges::contains(sources, source)) {
                sources.push_back(std::move(source));
            }
        }
        if(sources.empty()) {
            continue;
        }
        members.entries.push_back({
            .file = index.portable(file_table.resolve(path_id)),
            .sources = std::move(sources),
        });
    }
    for(auto file: provisional.files()) {
        members.provisional.push_back(index.portable(file_table.resolve(file)));
    }
    // Deterministic bytes: a save decides "unchanged" by byte equality.
    ranges::sort(members.entries, {}, &SavedMembers::Member::file);
    ranges::sort(members.provisional);
    return members;
}

void Project::reconcile_index(index::IndexReport& report) {
    // Without the persisted members nothing tells which databases listed a
    // unit, so none leaves the index. It is owed a save even when it
    // commits nothing else: waiting for an unrelated dirtying merge would
    // leave every following session unable to tell a removed unit.
    auto* persisted = saved_members.persisted();
    if(saved_members.unreadable()) {
        LOG_ERROR(
            "Index cache at {} cannot tell which databases listed its files; keeping every "
            "unit the build no longer compiles",
            std::string_view(settings.project.cache_dir));
    }
    llvm::StringMap<const SavedMembers::Member*> before;
    if(persisted) {
        for(auto& entry: persisted->entries) {
            before[entry.file] = &entry;
        }
        // Listed by other databases, a unit compiles as it did: only the
        // record follows the databases now listing it.
        for(auto& entry: recorded_members().entries) {
            if(auto it = before.find(entry.file);
               it != before.end() && it->second->sources != entry.sources) {
                saved_members.owe_save();
            }
        }
    } else {
        saved_members.owe_save();
    }

    // One the configuration stopped declaring is gone on purpose, and so is
    // one in a project nested in the root; one discovery no longer finds
    // may come back. Under discovery a database that is no longer
    // registered was either replaced by one that loaded, or merely vanished
    // and may come back.
    bool declared = rules.declares_sources();
    bool replaced = false;
    for(std::uint32_t i = 0; !declared && i < cdb.source_count(); i += 1) {
        replaced |= cdb.loaded(SourceID(i));
    }
    auto healthy = [&](const SavedMembers::Member& entry) {
        return ranges::all_of(entry.sources, [&](const std::string& source) {
            auto path = index.local(source);
            if(auto id = cdb.find_source(path)) {
                return cdb.loaded(*id);
            }
            return declared || replaced || !nested_project(root, CanonicalPath(path)).empty();
        });
    };

    llvm::SmallVector<Fid> units;
    index.for_each_unit([&](Fid unit) { units.push_back(unit); });
    for(auto unit: units) {
        auto path = file_table.resolve(unit);
        // An index built before the rule would otherwise serve on: the
        // background index never rebuilds an excluded unit. A build
        // compiling the unit indexes it all the same, for what its importers
        // or the units loading its header read, as a header's declarations
        // are through the units including it: that index stays.
        if(!rules.indexable(path) && !compiled_by_build(unit)) {
            LOG_INFO("A rule keeps {} out of the index; dropping its entries", path);
            index.drop(unit, report);
            continue;
        }
        // A header the units reading it index, which an older clice indexed
        // on its own when it was opened before them.
        if(!is_unit(unit) && is_header_path(path)) {
            LOG_INFO("The build no longer compiles {}; dropping its index", path);
            index.drop(unit, report);
            continue;
        }
        if(candidates(unit).empty()) {
            if(!persisted) {
                continue;
            }
            // A unit a database listed leaves when every database that
            // listed it loaded fine this session, as the live reload's
            // removed branch does; one that failed to load keeps its
            // last-known entries serving. Otherwise the default command
            // that covered it is gone.
            if(auto it = before.find(index.portable(path));
               it != before.end() && !it->second->sources.empty()) {
                if(!healthy(*it->second)) {
                    continue;
                }
                LOG_INFO("No compilation database lists {} any more; dropping its index", path);
            } else {
                LOG_INFO("The build no longer compiles {}; dropping its index", path);
            }
            index.drop(unit, report);
            continue;
        }
        // A unit deleted meanwhile keeps its shard behind (queries withhold
        // it) with nothing left to reindex.
        if(!index_command_holds(unit) && fs::exists(path)) {
            LOG_INFO("Compile command changed since the last session; reindexing {}", path);
            index.mark_outdated(unit);
            report.add_reindex(unit);
        }
    }
}

async::task<> Project::checkpoint() {
    if(store) {
        co_await async::queue([this] { store->checkpoint(); });
    }
    // A full search build or a fold here could still be running on the
    // thread pool when a shutdown cancels the checkpoint, holding up the
    // final save behind seconds of derived work: the pass end does them,
    // and a pass with nothing queued builds what the search index is owed.
    co_await save(index::SearchRebuild::Never);
    background_index.schedule();
    forget_evicted_pchs();
}

async::task<> Project::save(index::SearchRebuild search) {
    bool retry = true;
    bool owed = false;
    while(retry) {
        auto report = co_await write_database(search);
        retry = report.owes_retry;
        owed |= !report.reindex().empty();
        take_report(report);
        // No more indexing is queued, so no save is due for a while: the
        // file may shrink — unless a file was queued while the save ran. A
        // shrink that discarded the database owes the fresh one a save.
        if(!retry && search == index::SearchRebuild::Idle &&
           background_index.queue().queued() == 0) {
            report = co_await shrink_database();
            retry = report.owes_retry;
            owed |= !report.reindex().empty();
            take_report(report);
        }
    }
    // A save the compaction or a discarded database owed reindexes to queued
    // them: they need a pass of their own when none is running.
    if(owed) {
        background_index.schedule();
    }
}

void Project::take_report(const index::IndexReport& report) {
    for(auto id: report.reindex()) {
        background_index.enqueue(id, ReindexReason::ContentChanged);
    }
    if(!report.index_changed().empty()) {
        on_index_changed.emit(report.index_changed());
    }
}

chrono::milliseconds checkpoint_interval() {
    // Lets integration tests kill a server between checkpoints.
    if(auto ms = env_integer("CLICE_TEST_CHECKPOINT_MS")) {
        return chrono::milliseconds(*ms);
    }
    return chrono::minutes(5);
}

async::task<> Project::shutdown() {
    tree_tasks.cancel();
    co_await tree_tasks.join();
    co_await background_index.stop();
    co_await graph.shutdown();
    // Editors kill a server that is slow to exit, and building the search
    // index dominates a cold pass's save: the index commits with no more
    // than a run of the changes since the last save, and the next session's
    // first idle save builds the rest.
    co_await save(index::SearchRebuild::Never);
    // A worker still finishing a cancelled build then only fails its write.
    if(store) {
        store->shutdown();
    }
}

void Project::recover_corrupt_database(index::IndexReport& report) {
    LOG_WARN("Index database is corrupt; discarding it and rebuilding from scratch");
    for(auto unit: members()) {
        report.add_reindex(unit);
    }
    // The fresh database records the members at the next save.
    report.owes_retry = true;
    reopen_fresh_database();
}

void Project::reopen_fresh_database() {
    database->condemn();
    database.reset();
    // The index reads its records in place: it starts over with the
    // database, as does the record of the members it was saved with.
    index.clear();
    saved_members.forget_persisted();
    database = index::open_database(*store, database_key);
    // The metadata blobs died with the condemned database while their
    // loaded state lives on in memory; without a re-dirty the next save
    // skips them and a restart loses the user's context choices and every
    // rebuildable precompiled record.
    mark_precompiled_dirty();
    choices.dirty = !choices.bytes.empty();
}

bool Project::has_unsaved_state() const {
    return index.has_unsaved_state() || saved_members.owes_save(provisional.changes());
}

async::task<index::IndexReport> Project::write_database(index::SearchRebuild search) {
    index::IndexReport report;
    // A read-only session (batch lint, stats) keeps its metadata in memory
    // and exits with it — it must never write into a database a concurrent
    // writer owns.
    if(!database || database->read_only()) {
        co_return report;
    }
    co_await save_gate.acquire();
    auto gate = llvm::make_scope_exit([&] { save_gate.release(); });
    // Re-checked: the gate holder we just waited out may have hit
    // corruption and failed to reopen the database.
    if(!database || database->read_only()) {
        co_return report;
    }
    auto& db = *database;
    ScopedTimer timer;

    // A read since the last save met a damaged page or record: nothing more
    // goes into the database, which is discarded, whether or not anything
    // changed since.
    if(db.corrupted() || index.damaged()) {
        recover_corrupt_database(report);
        co_return report;
    }

    co_await index.update_search(search);
    index::SaveBatch batch;
    index.begin_save(batch, report);
    auto& blobs = batch.blobs;

    // The members commit in the same transaction as the index state they
    // describe. A save that commits nothing skips them (unless they are owed
    // a rewrite themselves): a pure CDB change dirties no blob on its own,
    // so the next dirtying save carries the fresh members.
    std::optional<std::string> members_bytes;
    if(!blobs.empty() || !batch.removals.empty() ||
       saved_members.owes_save(provisional.changes())) {
        members_bytes = saved_members.serialize(recorded_members(), provisional.changes());
        if(members_bytes) {
            blobs.push_back({index::BlobKind::Members, "cdb", copy_bytes(*members_bytes)});
        }
    }

    // Metadata blobs ride every save: dirty precompiled records and context
    // choices flush on whatever save runs next.
    bool carries_precompiled = false;
    bool carries_choices = false;
    if(precompiled_dirty) {
        if(auto bytes = serialize_precompiled_records(); !bytes.empty()) {
            carries_precompiled = true;
            blobs.push_back({index::BlobKind::PrecompiledRecords, "artifacts", copy_bytes(bytes)});
        }
    }
    if(choices.dirty && !choices.bytes.empty()) {
        carries_choices = true;
        blobs.push_back({index::BlobKind::ContextChoices, "contexts", copy_bytes(choices.bytes)});
    }
    // Serialization failures keep their dirty flag for the next attempt.
    if(carries_precompiled) {
        precompiled_dirty = false;
    }
    if(carries_choices) {
        choices.dirty = false;
    }

    if(blobs.empty() && batch.removals.empty()) {
        co_return report;
    }

    bool committed = false;
    // Cancellation while the write is still queued dequeues it (see
    // async::queue): the batch dies with this frame, so what it carried must
    // be changed again or the final shutdown save sees nothing to commit and
    // reports saved progress that is absent after restart.
    auto restore = [&] {
        index.abandon_save(batch);
        if(members_bytes) {
            saved_members.owe_save();
        }
        precompiled_dirty = precompiled_dirty || carries_precompiled;
        choices.dirty = choices.dirty || carries_choices;
    };
    auto cancelled = llvm::make_scope_exit(restore);
    co_await async::queue([&] { committed = db.write(blobs, batch.removals); });
    cancelled.release();

    if(!committed) {
        restore();
    } else if(members_bytes) {
        saved_members.saved(std::move(*members_bytes));
    }

    // Corruption can show up first at write time (a damaged page only the
    // write's tree descent reaches), or in a record this save read: heal
    // like load-time corruption instead of writing into the damaged
    // environment every save.
    if(db.corrupted() || index.damaged()) {
        recover_corrupt_database(report);
        co_return report;
    }

    if(!co_await index.migrate_views(db, *loop, report)) {
        recover_corrupt_database(report);
    }

    LOG_PERF("index",
             "phase=save blobs={} shards={} elapsed_ms={}",
             blobs.size(),
             batch.shards,
             timer.ms());
    co_return report;
}

bool shrink_due(const index::PageUsage& usage) {
    constexpr std::uint64_t worth = 64ull << 20;
    auto free = usage.free_pages * usage.page_size;
    return free >= worth && free * 4 >= usage.pages * usage.page_size;
}

async::task<index::IndexReport> Project::shrink_database() {
    index::IndexReport report;
    if(!database || database->read_only()) {
        co_return report;
    }
    co_await save_gate.acquire();
    auto gate = llvm::make_scope_exit([&] { save_gate.release(); });
    // Re-checked: the gate holder we waited out may have failed to reopen
    // the database. A damaged one is the next save's to discard.
    if(!database || database->corrupted() || index.damaged() || shrink_failed) {
        co_return report;
    }
    auto& db = *database;
    auto before = db.usage();
    if(!before || !shrink_due(*before)) {
        co_return report;
    }
    ScopedTimer timer;
    using Database = index::BlobDatabase;
    auto copy = Database::Copy::Failed;
    co_await async::queue([&] { copy = db.write_compact_copy(); });
    switch(copy) {
        case Database::Copy::Written: break;
        case Database::Copy::Busy: co_return report;
        case Database::Copy::Failed:
            shrink_failed = true;
            // The copy read every page: a damaged one it met is discarded
            // now, as a save meeting it would.
            if(db.corrupted()) {
                recover_corrupt_database(report);
            }
            co_return report;
    }
    // A handle lost in the swap lists no unit any more.
    llvm::SmallVector<Fid> units;
    index.for_each_unit([&](Fid unit) { units.push_back(unit); });
    switch(db.swap_compact_copy()) {
        case Database::Swap::Busy:
            LOG_INFO("Another process opened the index database; its file shrinks later");
            co_return report;
        case Database::Swap::Lost:
            recover_corrupt_database(report);
            for(auto unit: units) {
                report.add_reindex(unit);
            }
            co_return report;
        case Database::Swap::Kept: shrink_failed = true; break;
        case Database::Swap::Replaced: break;
    }
    if(!co_await index.rebind_views(db, *loop, /*at_once=*/true)) {
        recover_corrupt_database(report);
        co_return report;
    }
    auto after = db.usage();
    LOG_PERF("index",
             "phase=shrink before_mb={} after_mb={} elapsed_ms={}",
             before->pages * before->page_size >> 20,
             after ? after->pages * after->page_size >> 20 : 0,
             timer.ms());
    co_return report;
}

bool Project::load_saved(bool reconcile, index::IndexReport& report) {
    if(!database) {
        return true;
    }
    auto& db = *database;
    bool read_only = db.read_only();
    ScopedTimer timer;

    // The precompiled records and context choices are index-independent: they
    // load (and self-validate) beside any index state.
    auto load_metadata = [&] {
        if(auto records = db.read(index::BlobKind::PrecompiledRecords, "artifacts")) {
            load_precompiled_records(records.buffer->getBuffer());
        }
        if(auto choices = db.read(index::BlobKind::ContextChoices, "contexts")) {
            this->choices.bytes = choices.buffer->getBuffer().str();
        }
    };

    switch(index.open(db)) {
        // A database with no index yet may still hold the metadata and the
        // members of a session whose index was discarded.
        case index::LoadResult::Empty: break;
        case index::LoadResult::Unreadable:
            // Run this session memory-only — saving a fresh lineage over
            // records whose header was never read could alias their path
            // ids — and leave everything for a healthier restart to load.
            // Confirmed page corruption heals through rebuildability
            // instead: the condemned database deletes itself and a fresh
            // empty one opens in its place, so this session's rebuild
            // persists instead of being redone at the next start.
            if(!read_only && db.corrupted()) {
                LOG_WARN("Index database is corrupt; discarding it and rebuilding from scratch");
                reopen_fresh_database();
            } else {
                // Best effort before detaching: the metadata blobs may
                // still read while only the header is unreadable.
                load_metadata();
                LOG_WARN(
                    "Index database header unreadable; disabling index persistence this "
                    "session");
                database.reset();
            }
            return true;
        case index::LoadResult::Undecodable:
            // Another format discards the index; the metadata, read first,
            // goes into the fresh database at its first save.
            if(!read_only) {
                load_metadata();
                reopen_fresh_database();
            }
            return false;
        case index::LoadResult::Loaded: break;
    }
    load_metadata();
    if(reconcile) {
        reconcile_index(report);
    }

    // The reads above may have met a damaged page or record: heal it like
    // the unreadable-header case above instead of writing into a damaged
    // tree every session. The session starts over as a first run.
    if(!read_only && index.damaged()) {
        recover_corrupt_database(report);
        return true;
    }

    LOG_PERF("startup", "phase=index_load elapsed_ms={}", timer.ms());
    return true;
}

std::uint32_t PathTable::add(Fid file) {
    auto path = index.portable(files.resolve(file));
    auto [it, inserted] = indices.try_emplace(path, static_cast<std::uint32_t>(paths.size()));
    if(inserted) {
        paths.push_back(path);
    }
    return it->second;
}

std::optional<Fid> PathTable::file(std::uint32_t at) const {
    if(at >= paths.size() || paths[at].empty()) {
        return std::nullopt;
    }
    return files.intern(index.local(paths[at]));
}

std::string Project::serialize_precompiled_records() {
    Records data;
    data.pch_index_format = index::index_format_version;
    data.trials_scoring = feature::missing_context_scoring;
    PathTable table(index, file_table, data.paths);

    // Deps persist as (spelling, hash) pairs so they self-heal against any
    // index state. A version-less dep (missing or unhashable at capture)
    // writes hash 0 and reloads as one.
    auto dump_deps = [&](const Inputs& snap, std::vector<SavedInput>& out) {
        for(auto& dep: snap) {
            auto hash = dep.version.valid() ? file_table.version(dep.version).content_hash : 0;
            out.push_back({table.add(dep.file), hash, dep.absent});
        }
    };

    for(auto& e: pchs) {
        auto& st = e.second;
        if(st.path.empty())
            continue;
        SavedPCH entry;
        entry.key = e.getKey().str();
        entry.blob = st.blob;
        entry.bound = st.bound;
        entry.whole_file = st.whole_file;
        dump_deps(st.inputs, entry.deps);
        if(!st.base.empty()) {
            entry.base = st.base;
            entry.base_blob = st.base_blob;
        }
        data.pch.push_back(std::move(entry));
    }

    for(auto& [path_id, st]: pcms) {
        if(st.path.empty())
            continue;
        SavedPCM entry;
        entry.key = st.key;
        entry.blob = st.blob;
        entry.source_file = table.add(path_id);
        entry.module_name = dep_graph.module_of(path_id).str();
        dump_deps(st.inputs, entry.deps);
        data.pcm.push_back(std::move(entry));
    }

    hosting.self_containment.for_each([&](const SelfContainmentKey& key, const auto& entry) {
        SavedSelfContainment saved{
            .header = table.add(key.first),
            .command = key.second,
            .self_contained = entry.result.self_contained,
            .text = file_table.version(entry.result.text).content_hash,
            .disk = file_table.version(entry.result.disk).content_hash,
        };
        dump_deps(entry.inputs, saved.deps);
        data.trials.push_back(std::move(saved));
    });

    auto json = json::to_string(data, path_json);
    if(!json) {
        LOG_WARN("Failed to serialize the precompiled records");
        return {};
    }
    return std::move(*json);
}

void Project::load_precompiled_records(llvm::StringRef bytes) {
    if(bytes.empty() || !store) {
        return;
    }
    Records data;
    if(!json::from_string(bytes, data)) {
        LOG_WARN("Failed to parse the precompiled records");
        return;
    }

    PathTable table(index, file_table, data.paths);
    auto load_deps = [&](const std::vector<SavedInput>& dep_entries) -> Inputs {
        Inputs inputs;
        for(auto& dep: dep_entries) {
            auto dep_file = table.file(dep.path);
            if(!dep_file)
                continue;
            auto& input = inputs.emplace_back();
            input.file = *dep_file;
            input.absent = dep.missing;
            if(dep.hash != 0) {
                input.version = file_table.intern_version(input.file, dep.hash);
            }
        }
        return inputs;
    };

    bool pch_format_ok = data.pch_index_format == index::index_format_version;
    for(auto& entry: data.pch) {
        if(!pch_format_ok) {
            break;
        }
        if(!entry.blob) {
            store->invalidate("pch", entry.key);
            continue;
        }
        auto pch_path = store->lookup("pch", *entry.blob);
        if(!pch_path)
            continue;
        // A preamble's PCH without its pch.idx index is an incomplete pair
        // (crash between the two commits): drop it so the next compile
        // rebuilds both.
        std::optional<std::string> index_path;
        if(!entry.whole_file) {
            index_path = store->lookup_aux("pch", *entry.blob);
            if(!index_path) {
                store->invalidate("pch", *entry.blob);
                continue;
            }
        }

        auto& st = pchs[entry.key];
        st.blob = std::move(*entry.blob);
        st.path = *pch_path;
        st.bound = entry.bound;
        st.whole_file = entry.whole_file;
        st.inputs = load_deps(entry.deps);
        st.base = entry.base.value_or("");
        st.base_blob = entry.base_blob.value_or("");
        st.index_path = index_path.value_or("");
    }

    for(auto& entry: data.pcm) {
        if(!entry.blob) {
            store->invalidate("pcm", entry.key);
            continue;
        }
        auto pcm_path = store->lookup("pcm", *entry.blob);
        // A dep-less entry is an unvalidatable snapshot that would blindly
        // serve a stale PCM (the key embeds no content). Drop it and let
        // the module rebuild once.
        if(!pcm_path || entry.deps.empty())
            continue;
        auto source = table.file(entry.source_file);
        if(!source)
            continue;
        auto& st = pcms[*source];
        st.path = *pcm_path;
        st.key = entry.key;
        st.blob = std::move(*entry.blob);
        st.inputs = load_deps(entry.deps);
    }

    if(data.trials_scoring != feature::missing_context_scoring) {
        data.trials.clear();
    }
    for(auto& saved: data.trials) {
        auto header = table.file(saved.header);
        if(!header) {
            continue;
        }
        hosting.self_containment.commit({*header, saved.command},
                                        {.self_contained = saved.self_contained,
                                         .text = file_table.intern_version(*header, saved.text),
                                         .disk = file_table.intern_version(*header, saved.disk)},
                                        load_deps(saved.deps));
    }

    LOG_INFO("Loaded precompiled records: {} PCH entries, {} PCM entries",
             pchs.size(),
             pcms.size());
}

}  // namespace clice
