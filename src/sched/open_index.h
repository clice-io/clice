#pragma once

#include <optional>
#include <string>

#include "sched/command_resolver.h"
#include "sched/workspace.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

struct ContextsOwner;

/// Open a workspace's persisted index for reading: the configuration
/// resolved, the cache store and the blob database opened read-only, the
/// global table and the search index bound in place from the database's
/// read snapshot (pinned for the workspace's lifetime), shards fetched
/// on first use. Nothing is decoded or copied. False — with the cause
/// logged — when there is no usable index.
bool open_index(Workspace& workspace,
                llvm::StringRef root,
                llvm::StringRef requested_configuration);

/// What load_index found beyond the tables: the translation units the
/// load dropped as stale or partially written, whose rows are absent
/// until a reindex lands.
struct LoadedIndex {
    llvm::SmallVector<Fid> dropped;
};

/// Open the index as the writer loads it — every manifest adopted, every
/// shard fetched and verified, the header-mode verdicts restored into
/// `commands` and the persisted context choices into `contexts` when
/// given — for the commands that walk the whole index or need the build
/// (loaded when `with_build`). Nullopt, with the cause logged, when there
/// is no usable index.
std::optional<LoadedIndex> load_index(Workspace& workspace,
                                      CommandResolver& commands,
                                      ContextsOwner* contexts,
                                      llvm::StringRef root,
                                      llvm::StringRef requested_configuration,
                                      bool with_build);

/// An inspected file as the index keys it: a relative argument names a
/// file under the workspace, whatever the process working directory, and
/// dot segments are folded the way the compiler's paths were.
std::string inspected_path(const Workspace& workspace, llvm::StringRef argument);

}  // namespace clice
