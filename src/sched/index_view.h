#pragma once

#include <cstddef>
#include <string>

#include "sched/context.h"
#include "sched/index/store.h"
#include "sched/workspace.h"

#include "kota/async/async.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// The persisted index of a workspace as a short-lived command reads it:
/// opened read-only, never touching the disk, its shards borrowed from
/// the database's read snapshot for the process's lifetime.
struct IndexView {
    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts{workspace};
    IndexStore store{loop, workspace, contexts};
    std::string configuration;

    /// Translation units the load dropped as stale or partially written:
    /// their rows are absent until a reindex lands.
    llvm::SmallVector<Fid> dropped;

    const index::ProjectIndex& project() const {
        return workspace.project_index;
    }

    llvm::StringRef path_of(Fid file) const {
        return workspace.file_table.resolve(file);
    }
};

/// An inspected file as the index keys it: a relative argument names a
/// file under the workspace, whatever the process working directory, and
/// dot segments are folded the way the compiler's paths were.
std::string inspected_path(const IndexView& view, llvm::StringRef argument);

struct IndexViewOptions {
    /// Also load the build: the compilation databases and the include
    /// graph scanned from their units, for the questions the index alone
    /// cannot answer (compile commands, the build's file set, include
    /// dependencies).
    bool with_build = false;
};

/// Run `report` over the workspace's persisted index, opened read-only
/// with the mid-save retry. Returns the report's exit code, or the open
/// failure's with the cause already logged.
int with_index(llvm::StringRef root,
               llvm::StringRef configuration,
               IndexViewOptions options,
               llvm::function_ref<int(IndexView&)> report);

}  // namespace clice
