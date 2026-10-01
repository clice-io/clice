#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "project/cdb_watcher.h"
#include "project/project.h"
#include "server/invalidator.h"
#include "server/session_store.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

/// Polling of what a project is built from, beyond the files themselves
/// (which vfs::DiskState looks at): its compilation databases (see
/// CDBWatcher), the sources its default-command rules claim, and the git
/// checkout the workspace lies in — every git operation that changes the
/// worktree rewrites the index, and then every file under the workspace
/// is due for a look.
///
/// The tracker only observes; it never dispatches. Database and source
/// changes come back as the events the polling loops (and the
/// clice/internal/poll test hook) hand to dispatch(), which keeps the
/// tracker unit-testable against plain data structures.
class FileTracker {
public:
    /// Construct after the project is loaded: its databases are baselined
    /// at their loads.
    FileTracker(Project& project, const SessionStore& store, CanonicalPath root);

    /// One CDB poll tick (see CDBWatcher::tick), the open files looking
    /// for a database; the reload's diff as one CDBChanged event.
    llvm::SmallVector<FileEvent> tick_cdb(bool force = false);

    /// See CDBWatcher::discover_around; the loads' diffs as CDBChanged
    /// events.
    llvm::SmallVector<FileEvent> discover_around(Fid path_id);

    /// A file created under a default-command rule joins the build: the
    /// gain reported as a CDBChanged event, as a database reload reports
    /// an added command. One deleted leaves through DiskRemoved, like any
    /// file.
    llvm::SmallVector<FileEvent> tick_sources();

private:
    Project& project;
    const SessionStore& store;
    CDBWatcher cdb;

    /// The checkout's HEAD and index.
    llvm::SmallVector<std::shared_ptr<const vfs::Flag>> checkout;
};

}  // namespace clice
