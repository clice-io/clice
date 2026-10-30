module clice:project.save;

import :index.database;
import :index.project_index;
import :project.project;
import :support.prelude;

namespace clice {

/// What load_saved found.
struct SavedLoad {
    /// False when the database's header is of another format or corrupt:
    /// the writer discards the database and starts over, but a reader must
    /// report an unusable cache instead of an empty index.
    bool decoded = true;

    /// The units owed a reindex the load found: the ones a rule keeps out or
    /// whose command changed while no server ran.
    index::IndexReport report;
};

/// Open what the project's database holds: the index (ProjectIndex::open),
/// the artifact records into the project, the context choices; then, with
/// `reconcile` (the build is loaded), drop the index of the units a rule
/// keeps out and of those whose command changed while no server ran
/// (CommandSnapshot::reconcile) — in memory only on a read-only database.
/// The writer replaces a database of another format or found corrupt by a
/// fresh one; one whose header cannot be read is detached, persistence off
/// for the session.
SavedLoad load_saved(Project& project, bool reconcile);

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
