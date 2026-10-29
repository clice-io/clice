module clice;

import :index.database;
import :index.project_index;
import :project.artifact_records;
import :project.command_snapshot;
import :project.project;
import :project.save;
import :support.bytes;
import :support.logging;
import :support.timer;

namespace clice {

namespace {

/// Wake the context choices' durability waiters (switchContext).
void pulse(ContextChoices& contexts) {
    contexts.committed.set();
    contexts.committed.reset();
}

}  // namespace

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
    project.contexts.dirty = !project.contexts.bytes.empty();
    // Durability waiters re-evaluate against the new database: a failed
    // reopen disables persistence for the session, and a parked
    // switchContext would otherwise sleep forever — no later save pulses,
    // they all early-return on the null database.
    pulse(project.contexts);
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
    auto& contexts = project.contexts;
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
    // dirties no blob on its own — the invalidator's drops do — so the
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
    // choices flush on whatever save runs next. The context choices' change number
    // is taken here — marks arriving past this point are not covered by
    // these bytes and wait for the next save. The context choices' fate
    // is tracked on its own: failing artifact records must not park a
    // switchContext ack whose choice is already durable.
    auto flush_seq = contexts.save_seq;
    bool carries_artifacts = false;
    bool carries_contexts = false;
    bool contexts_ok = true;
    if(project.artifacts_dirty) {
        if(auto bytes = serialize_artifact_records(project); !bytes.empty()) {
            carries_artifacts = true;
            blobs.push_back({index::BlobKind::ArtifactRecords, "artifacts", copy_bytes(bytes)});
        }
    }
    if(contexts.dirty) {
        if(!contexts.bytes.empty()) {
            carries_contexts = true;
            blobs.push_back(
                {index::BlobKind::ContextChoices, "contexts", copy_bytes(contexts.bytes)});
        } else {
            contexts_ok = false;
        }
    }
    // Serialization failures keep their dirty flag for the next attempt.
    if(carries_artifacts) {
        project.artifacts_dirty = false;
    }
    if(carries_contexts) {
        contexts.dirty = false;
    }

    if(blobs.empty() && batch.removals.empty()) {
        // A serialize-only failure builds no batch: waiters still must see
        // the failed attempt (see the failure pulse below), or a request
        // bounded on failed saves never counts one.
        if(!contexts_ok) {
            pulse(contexts);
        }
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
        contexts.dirty = contexts.dirty || carries_contexts;
    };
    auto cancelled = llvm::make_scope_exit(restore);
    co_await kota::queue([&] { committed = db.write(blobs, batch.removals); });
    cancelled.release();

    if(!committed) {
        restore();
        contexts_ok = false;
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

    if(co_await index.migrate_views(db, loop, report)) {
        // Context choices as of the number taken are durable: advance the
        // committed number and wake durability waiters (switchContext).
        // Failed attempts pulse too, or a permanently failing write (full
        // disk, unserializable path) parks every durability wait forever;
        // they give up after a few of these. A recovery pulses as it
        // reopens: what the condemned database took is not durable.
        if(contexts_ok) {
            contexts.committed_seq = std::max(contexts.committed_seq, flush_seq);
        }
        pulse(contexts);
    } else {
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

}  // namespace clice
