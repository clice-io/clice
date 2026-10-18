module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.database;
import :index.project_index;
import :project.artifact_records;
import :project.command_resolver;
import :project.command_snapshot;
import :project.project;
import :project.save;
import :support.logging;
import :support.timer;

namespace clice {

namespace {

/// Wake the context choices' durability waiters (switchContext).
void pulse(ContextChoices& contexts) {
    contexts.committed.set();
    contexts.committed.reset();
}

/// The recovery from a database found corrupt after it was opened: nothing
/// in it survives, so the index re-persists what it holds into the database
/// opened in its place, as does the command snapshot.
void recover_corrupt_database(Project& project, index::IndexReport& report) {
    project.index.recover_corrupt_database(report);
    project.command_snapshot.forget_persisted();
    // The snapshot just persisted (if any) predates this recovery's debt
    // and lives in a condemned database anyway: one metadata retry makes
    // the fresh database record it.
    report.owes_retry = true;
    reopen_fresh_database(project);
}

}  // namespace

void reopen_fresh_database(Project& project) {
    LOG_WARN("Index database is corrupt; discarding it and rebuilding from scratch");
    project.database->condemn();
    project.database.reset();
    project.database = index::open_database(*project.store, project.build.active_configuration());
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
                                            CommandResolver& commands,
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

    co_await index.rebuild_search(search);
    index::SaveBatch batch;
    index.begin_save(batch, report);
    auto& blobs = batch.blobs;

    // The command snapshot goes last, after the whole index state it
    // describes: committed before the project table, a crash between the two
    // would leave the old project table still pinning old-command manifests
    // under a snapshot that already matches the live CDB — the load's diff
    // would then never drop them. A save that commits nothing skips it
    // (unless the snapshot itself is owed a rewrite): a pure CDB change
    // dirties no blob on its own — the invalidator's drops do — so the
    // next dirtying save carries the fresh snapshot. The debt input is the
    // pump's snapshot at call time plus the owner debt the compaction
    // discovered; anything showing up past this point is too late for
    // these bytes and returns in the report.
    std::optional<std::string> snapshot_bytes;
    std::optional<std::size_t> snapshot_slot;
    if(!blobs.empty() || !batch.removals.empty() || snapshot.owes_save(project)) {
        debt.append(report.reindex().begin(), report.reindex().end());
        snapshot_bytes = snapshot.serialize(project, debt);
        if(snapshot_bytes) {
            snapshot_slot = blobs.size();
            blobs.push_back({index::BlobKind::CommandSnapshot, "cdb", *snapshot_bytes});
        }
    }

    // Metadata blobs ride every save: dirty artifact records and context
    // choices flush on whatever save runs next. The context choices' ticket
    // is taken here — marks arriving past this point are not covered by
    // these bytes and wait for the next save. The context choices' fate
    // is tracked on its own: failing artifact records must not park a
    // switchContext ack whose choice is already durable.
    auto flush_ticket = contexts.ticket;
    std::optional<std::size_t> artifacts_slot;
    std::optional<std::size_t> contexts_slot;
    bool contexts_ok = true;
    if(project.artifacts_dirty) {
        if(auto bytes = serialize_artifact_records(project, commands); !bytes.empty()) {
            artifacts_slot = blobs.size();
            blobs.push_back({index::BlobKind::ArtifactRecords, "artifacts", std::move(bytes)});
        }
    }
    if(contexts.dirty) {
        if(!contexts.bytes.empty()) {
            contexts_slot = blobs.size();
            blobs.push_back({index::BlobKind::ContextChoices, "contexts", contexts.bytes});
        } else {
            contexts_ok = false;
        }
    }
    // Serialization failures keep their dirty flag for the next attempt.
    if(artifacts_slot) {
        project.artifacts_dirty = false;
    }
    if(contexts_slot) {
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

    llvm::SmallVector<std::size_t> failed;
    // Cancellation while the write is still queued dequeues it (see
    // kota::queue): the batch dies with this frame, so what it carried must
    // be dirty again or the final shutdown save sees nothing to commit and
    // reports saved progress that is absent after restart.
    auto restore = llvm::make_scope_exit([&] {
        index.abandon_save(batch);
        if(snapshot_slot) {
            snapshot.unsaved();
        }
        project.artifacts_dirty = project.artifacts_dirty || artifacts_slot.has_value();
        contexts.dirty = contexts.dirty || contexts_slot.has_value();
    });
    co_await kota::queue([&] { failed = db.write(blobs, batch.removals); });
    restore.release();

    for(auto i: failed) {
        if(snapshot_slot && i == *snapshot_slot) {
            // Keep the old persisted bytes and stay dirty so the next
            // save retries even when nothing else changes by then.
            snapshot.unsaved();
            snapshot_slot.reset();
        } else if(artifacts_slot && i == *artifacts_slot) {
            project.artifacts_dirty = true;
        } else if(contexts_slot && i == *contexts_slot) {
            contexts.dirty = true;
            contexts_ok = false;
        }
    }
    index.finish_save(batch, failed);
    if(snapshot_slot) {
        snapshot.saved(std::move(*snapshot_bytes));
    }

    // Context choices as of the ticket taken are durable: advance the
    // committed ticket and wake durability waiters (switchContext). Failed
    // attempts pulse too, or a permanently failing write (full disk,
    // unserializable path) parks every durability wait forever; they give
    // up after a few of these.
    if(contexts_ok) {
        contexts.committed_ticket = std::max(contexts.committed_ticket, flush_ticket);
    }
    pulse(contexts);

    // Corruption can show up first at write time (a damaged page only the
    // write's tree descent reaches): heal like load-time corruption instead
    // of writing into the damaged environment every save. The batch's
    // shards — dirty at batch build, hence memory-backed — re-dirty before
    // the recovery's shed so they survive it and re-persist wholesale.
    if(db.corrupted()) {
        index.redirty(batch);
        recover_corrupt_database(project, report);
        co_return report;
    }

    if(!co_await index.migrate_views(db, loop, report)) {
        recover_corrupt_database(project, report);
    }

    LOG_PERF("index",
             "phase=save shards={} manifests={} total={} elapsed_ms={}",
             batch.shard_ids.size(),
             batch.manifest_ids.size(),
             index.shard_count(),
             timer.ms());
    co_return report;
}

}  // namespace clice
