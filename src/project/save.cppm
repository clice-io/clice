module clice:project.save;

import :index.database;
import :index.project_index;
import :project.project;
import :support.prelude;

namespace clice {

/// Persist what the project's database carries: the index's dirty state
/// (ProjectIndex::begin_save), then the command snapshot, the artifact
/// records and the context choices, in one write offloaded to the kota
/// thread pool. What the write fails to commit stays dirty for the next
/// save; a database the write finds corrupt is replaced by a fresh one.
///
/// `debt` is the pump's immutable debt snapshot at call time: the command
/// snapshot persists the entries of units without database entries so a
/// dropped one's repair debt survives a process exit. Owner debt the compaction discovers before
/// serializing joins the same snapshot; debt that shows up after it
/// (write-time corruption recovery) comes back in the report with
/// owes_retry set.
kota::task<index::IndexReport>
    save_project(Project& project,
                 kota::event_loop& loop,
                 llvm::SmallVector<Fid> debt,
                 index::SearchRebuild search = index::SearchRebuild::Behind);

/// Whether state remains that no save committed. After a final save this
/// means write failures whose retry never came — the one-shot `clice index`
/// must not report a durable index from this.
bool has_unsaved_state(const Project& project);

/// The recovery from a database found corrupt after it was opened —
/// damaged pages, or a record failing its checks: nothing in it survives,
/// so the project starts over as a first run, every unit of the build and
/// every unit of its own the index held owed a reindex.
void recover_corrupt_database(Project& project, index::IndexReport& report);

/// Whether a database file's free pages are worth a compact copy: a quarter
/// of the file, and 64 MB at least.
bool shrink_due(const index::PageUsage& usage);

/// Replace the database file by a compact copy when shrink_due, no save
/// running meanwhile, and move the index's views onto it. Meant for when
/// indexing is idle: the copy takes seconds on a large project, and it
/// replaces the file only while no other process has the database open —
/// else the file shrinks at a later call.
kota::task<index::IndexReport> shrink_database(Project& project, kota::event_loop& loop);

/// Confirmed corruption heals through rebuildability: condemn the corrupt
/// database (deleted on close) and continue on a freshly opened empty
/// one, the index and the persisted command snapshot starting over with
/// it, so the session's rebuild persists instead of waiting for the next
/// start. A failed reopen leaves persistence disabled for the session.
void reopen_fresh_database(Project& project);

}  // namespace clice
