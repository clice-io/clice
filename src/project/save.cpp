module clice;

import :index.database;
import :index.project_index;
import :project.artifact_records;
import :project.build;
import :project.command_snapshot;
import :project.project;
import :project.save;
import :support.bytes;
import :support.logging;
import :support.timer;

namespace clice {

void recover_corrupt_database(Project& project, index::IndexReport& report) {
    LOG_WARN("Index database is corrupt; discarding it and rebuilding from scratch");
    for(auto unit: project.build.members()) {
        report.add_reindex(unit);
    }
    // No pass over the build reaches a unit of its own (a header indexed
    // with a borrowed command): as many as the records still name.
    project.index.for_each_unit([&](Fid unit) {
        if(project.build.entries(unit).empty()) {
            report.add_reindex(unit);
        }
    });
    // The fresh database records the command snapshot at the next save.
    report.owes_retry = true;
    reopen_fresh_database(project);
}

void reopen_fresh_database(Project& project) {
    project.database->condemn();
    project.database.reset();
    // The index reads its records in place: it starts over with the
    // database, as does the snapshot of the commands that built it.
    project.index.clear();
    project.command_snapshot.forget_persisted();
    project.database = index::open_database(*project.store, project.database_key);
    // The metadata blobs died with the condemned database while their
    // loaded state lives on in memory; without a re-dirty the next save
    // skips them and a restart loses the user's context choices and every
    // rebuildable artifact record.
    project.mark_artifacts_dirty();
    project.choices.dirty = !project.choices.bytes.empty();
}

bool has_unsaved_state(const Project& project) {
    return project.index.has_unsaved_state() || project.command_snapshot.owes_save(project);
}

kota::task<index::IndexReport> save_project(Project& project,
                                            kota::event_loop& loop,
                                            llvm::SmallVector<Fid> debt,
                                            index::SearchRebuild search) {
    index::IndexReport report;
    // A read-only session (batch lint, stats) keeps its metadata in memory
    // and exits with it — it must never write into a database a concurrent
    // writer owns.
    if(!project.database || project.database->read_only()) {
        co_return report;
    }
    co_await project.save_gate.acquire();
    auto gate = llvm::make_scope_exit([&] { project.save_gate.release(); });
    // Re-checked: the gate holder we just waited out may have hit
    // corruption and failed to reopen the database.
    if(!project.database || project.database->read_only()) {
        co_return report;
    }
    auto& db = *project.database;
    auto& index = project.index;
    auto& snapshot = project.command_snapshot;
    auto& choices = project.choices;
    ScopedTimer timer;

    // A read since the last save met a damaged page or record: nothing more
    // goes into the database, which is discarded, whether or not anything
    // changed since.
    if(db.corrupted() || index.damaged()) {
        recover_corrupt_database(project, report);
        co_return report;
    }

    co_await index.update_search(search);
    index::SaveBatch batch;
    index.begin_save(batch, report);
    auto& blobs = batch.blobs;

    // The command snapshot commits in the same transaction as the index
    // state it describes. A save that commits nothing skips it (unless the
    // snapshot itself is owed a rewrite): a pure CDB change
    // dirties no blob on its own — the index drops it causes do — so the
    // next dirtying save carries the fresh snapshot. The debt input is the
    // pump's snapshot at call time plus the owner debt the compaction
    // discovered; anything showing up past this point is too late for
    // these bytes and returns in the report.
    std::optional<std::string> snapshot_bytes;
    if(!blobs.empty() || !batch.removals.empty() || snapshot.owes_save(project)) {
        debt.append(report.reindex().begin(), report.reindex().end());
        snapshot_bytes = snapshot.serialize(project, debt);
        if(snapshot_bytes) {
            blobs.push_back({index::BlobKind::CommandSnapshot, "cdb", copy_bytes(*snapshot_bytes)});
        }
    }

    // Metadata blobs ride every save: dirty artifact records and context
    // choices flush on whatever save runs next.
    bool carries_artifacts = false;
    bool carries_choices = false;
    if(project.artifacts_dirty) {
        if(auto bytes = serialize_artifact_records(project); !bytes.empty()) {
            carries_artifacts = true;
            blobs.push_back({index::BlobKind::ArtifactRecords, "artifacts", copy_bytes(bytes)});
        }
    }
    if(choices.dirty && !choices.bytes.empty()) {
        carries_choices = true;
        blobs.push_back({index::BlobKind::ContextChoices, "contexts", copy_bytes(choices.bytes)});
    }
    // Serialization failures keep their dirty flag for the next attempt.
    if(carries_artifacts) {
        project.artifacts_dirty = false;
    }
    if(carries_choices) {
        choices.dirty = false;
    }

    if(blobs.empty() && batch.removals.empty()) {
        co_return report;
    }

    bool committed = false;
    // Cancellation while the write is still queued dequeues it (see
    // kota::queue): the batch dies with this frame, so what it carried must
    // be changed again or the final shutdown save sees nothing to commit and
    // reports saved progress that is absent after restart.
    auto restore = [&] {
        index.abandon_save(batch);
        if(snapshot_bytes) {
            snapshot.unsaved();
        }
        project.artifacts_dirty = project.artifacts_dirty || carries_artifacts;
        choices.dirty = choices.dirty || carries_choices;
    };
    auto cancelled = llvm::make_scope_exit(restore);
    co_await kota::queue([&] { committed = db.write(blobs, batch.removals); });
    cancelled.release();

    if(!committed) {
        restore();
    } else if(snapshot_bytes) {
        snapshot.saved(std::move(*snapshot_bytes));
    }

    // Corruption can show up first at write time (a damaged page only the
    // write's tree descent reaches), or in a record this save read: heal
    // like load-time corruption instead of writing into the damaged
    // environment every save.
    if(db.corrupted() || index.damaged()) {
        recover_corrupt_database(project, report);
        co_return report;
    }

    if(!co_await index.migrate_views(db, loop, report)) {
        recover_corrupt_database(project, report);
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

kota::task<index::IndexReport> shrink_database(Project& project, kota::event_loop& loop) {
    index::IndexReport report;
    if(!project.database || project.database->read_only()) {
        co_return report;
    }
    co_await project.save_gate.acquire();
    auto gate = llvm::make_scope_exit([&] { project.save_gate.release(); });
    // Re-checked: the gate holder we waited out may have failed to reopen
    // the database. A damaged one is the next save's to discard.
    if(!project.database || project.database->corrupted() || project.index.damaged() ||
       project.shrink_failed) {
        co_return report;
    }
    auto& db = *project.database;
    auto before = db.usage();
    if(!before || !shrink_due(*before)) {
        co_return report;
    }
    ScopedTimer timer;
    using Database = index::BlobDatabase;
    auto copy = Database::Copy::Failed;
    co_await kota::queue([&] { copy = db.write_compact_copy(); });
    switch(copy) {
        case Database::Copy::Written: break;
        case Database::Copy::Busy: co_return report;
        case Database::Copy::Failed:
            project.shrink_failed = true;
            // The copy read every page: a damaged one it met is discarded
            // now, as a save meeting it would.
            if(db.corrupted()) {
                recover_corrupt_database(project, report);
            }
            co_return report;
    }
    // A handle lost in the swap lists no unit any more.
    llvm::SmallVector<Fid> units;
    project.index.for_each_unit([&](Fid unit) { units.push_back(unit); });
    switch(db.swap_compact_copy()) {
        case Database::Swap::Busy:
            LOG_INFO("Another process opened the index database; its file shrinks later");
            co_return report;
        case Database::Swap::Lost:
            recover_corrupt_database(project, report);
            for(auto unit: units) {
                report.add_reindex(unit);
            }
            co_return report;
        case Database::Swap::Kept: project.shrink_failed = true; break;
        case Database::Swap::Replaced: break;
    }
    if(!co_await project.index.rebind_views(db, loop, /*at_once=*/true)) {
        recover_corrupt_database(project, report);
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

/// Drop the index of every TU a rule's `index = false` keeps out: an index
/// built before the rule would otherwise serve on, since the pump never
/// rebuilds an excluded unit.
static void drop_excluded(Project& project, index::IndexReport& report) {
    llvm::SmallVector<Fid> excluded;
    project.index.for_each_unit([&](Fid tu) {
        if(!project.build.indexed(project.file_table.resolve(tu))) {
            excluded.push_back(tu);
        }
    });
    for(auto tu: excluded) {
        LOG_INFO("A rule keeps {} out of the index; dropping its entries",
                 project.file_table.resolve(tu));
        project.index.drop(tu, report);
    }
}

SavedLoad load_saved(Project& project, bool reconcile) {
    SavedLoad result;
    auto& report = result.report;
    if(!project.database) {
        return result;
    }
    auto& db = *project.database;
    auto& index = project.index;
    bool read_only = db.read_only();
    ScopedTimer timer;

    // Artifact metadata and context choices are index-independent: they
    // load (and self-validate) beside any index state.
    auto load_metadata = [&] {
        if(auto artifacts = db.read(index::BlobKind::ArtifactRecords, "artifacts")) {
            load_artifact_records(project, artifacts.buffer->getBuffer());
        }
        if(auto choices = db.read(index::BlobKind::ContextChoices, "contexts")) {
            project.choices.bytes = choices.buffer->getBuffer().str();
        }
    };

    switch(index.open(db)) {
        // A database with no index yet may still hold the command snapshot,
        // and with it the debt of the files a save owed an index: the
        // snapshot of a session whose index was discarded.
        case index::LoadOutcome::Empty: break;
        case index::LoadOutcome::Unreadable:
            // Run this session memory-only — saving a fresh lineage over
            // records whose header was never read could alias their path
            // ids — and leave everything for a healthier restart to load.
            // Confirmed page corruption heals through rebuildability
            // instead: the condemned database deletes itself and a fresh
            // empty one opens in its place, so this session's rebuild
            // persists instead of being redone at the next start.
            if(!read_only && db.corrupted()) {
                LOG_WARN("Index database is corrupt; discarding it and rebuilding from scratch");
                reopen_fresh_database(project);
            } else {
                // Best effort before detaching: the metadata blobs may
                // still read while only the header is unreadable.
                load_metadata();
                LOG_WARN(
                    "Index database header unreadable; disabling index persistence this "
                    "session");
                project.database.reset();
            }
            return result;
        case index::LoadOutcome::Undecodable:
            // Another format discards everything, the metadata with it.
            result.decoded = false;
            if(!read_only) {
                reopen_fresh_database(project);
            }
            return result;
        case index::LoadOutcome::Loaded: break;
    }
    load_metadata();
    if(reconcile) {
        drop_excluded(project, report);
        project.command_snapshot.reconcile(project, report);
    }

    // The reads above may have met a damaged page or record: heal it like
    // the unreadable-header case above instead of writing into a damaged
    // tree every session. The session starts over as a first run.
    if(!read_only && index.damaged()) {
        recover_corrupt_database(project, report);
        return result;
    }

    LOG_PERF("startup", "phase=index_load elapsed_ms={}", timer.ms());
    return result;
}

}  // namespace clice
