module clice;

import :index.database;
import :index.project_index;
import :project.indexer;
import :project.listings;
import :project.project;
import :support.bytes;
import :support.environment;
import :support.logging;
import :support.timer;

namespace clice {

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
    forget_evicted();
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
    if(serving && !report.index_changed().empty()) {
        serving->index_changed(report.index_changed());
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
    // The fresh database records the listings at the next save.
    report.owes_retry = true;
    reopen_fresh_database();
}

void Project::reopen_fresh_database() {
    database->condemn();
    database.reset();
    // The index reads its records in place: it starts over with the
    // database, as do the listings it was saved with.
    index.clear();
    listings.forget_persisted();
    database = index::open_database(*store, database_key);
    // The metadata blobs died with the condemned database while their
    // loaded state lives on in memory; without a re-dirty the next save
    // skips them and a restart loses the user's context choices and every
    // rebuildable precompiled record.
    mark_precompiled_dirty();
    choices.dirty = !choices.bytes.empty();
}

bool Project::has_unsaved_state() const {
    return index.has_unsaved_state() || listings.owes_save(provisional.changes());
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

    // The listings commit in the same transaction as the index state they
    // describe. A save that commits nothing skips them (unless they are owed
    // a rewrite themselves): a pure CDB change dirties no blob on its own,
    // so the next dirtying save carries the fresh listings.
    std::optional<std::string> listings_bytes;
    if(!blobs.empty() || !batch.removals.empty() || listings.owes_save(provisional.changes())) {
        listings_bytes = listings.serialize(listed(), provisional.changes());
        if(listings_bytes) {
            blobs.push_back({index::BlobKind::Listings, "cdb", copy_bytes(*listings_bytes)});
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
        if(listings_bytes) {
            listings.owe_save();
        }
        precompiled_dirty = precompiled_dirty || carries_precompiled;
        choices.dirty = choices.dirty || carries_choices;
    };
    auto cancelled = llvm::make_scope_exit(restore);
    co_await async::queue([&] { committed = db.write(blobs, batch.removals); });
    cancelled.release();

    if(!committed) {
        restore();
    } else if(listings_bytes) {
        listings.saved(std::move(*listings_bytes));
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
        // listings of a session whose index was discarded.
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

}  // namespace clice
