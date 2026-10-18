module;

#include "modules/prelude.h"

module clice:project.save;

import :index.project_index;
import :project.command_resolver;
import :project.project;

namespace clice {

/// Persist what the project's database carries: the index's dirty state
/// (ProjectIndex::begin_save), then the command snapshot, the artifact
/// records and the context choices, in one write offloaded to the kota
/// thread pool. What the write fails to commit stays dirty for the next
/// save; a database the write finds corrupt is replaced by a fresh one.
///
/// `debt` is the pump's immutable debt snapshot at call time: the command
/// snapshot persists its standalone entries so a dropped header's repair
/// debt survives a process exit. Owner debt the compaction discovers before
/// serializing joins the same snapshot; debt that shows up after it
/// (write-time corruption recovery) comes back in the report with
/// owes_retry set.
kota::task<index::IndexReport>
    save_project(Project& project,
                 CommandResolver& commands,
                 kota::event_loop& loop,
                 llvm::SmallVector<Fid> debt,
                 index::SearchRebuild search = index::SearchRebuild::Behind);

/// Whether state remains that no save committed. After a final save this
/// means write failures whose retry never came — the one-shot `clice index`
/// must not report a durable index from this.
bool has_unsaved_state(const Project& project);

/// Confirmed corruption heals through rebuildability: condemn the
/// database (deleted on close) and continue on a freshly opened empty
/// one, so the session's rebuild persists instead of waiting for the
/// next start. A failed reopen leaves persistence disabled for the
/// session.
void reopen_fresh_database(Project& project);

}  // namespace clice
