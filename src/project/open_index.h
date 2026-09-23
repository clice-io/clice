#pragma once

#include <optional>
#include <string>

#include "project/command_resolver.h"
#include "project/project.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// Open a project's persisted index for reading: the configuration
/// resolved, the cache store and the blob database opened read-only, the
/// global table and the search index bound in place from the database's
/// read snapshot (pinned for the project's lifetime), shards fetched
/// on first use. Nothing is decoded or copied. False — with the cause
/// logged — when there is no usable index.
bool open_index(Project& project, llvm::StringRef root, llvm::StringRef requested_configuration);

/// What load_index found beyond the tables: the translation units the
/// load dropped as stale or partially written, whose rows are absent
/// until a reindex lands, and the persisted context choices.
struct LoadedIndex {
    llvm::SmallVector<Fid> dropped;

    /// The contexts blob as loaded (see ContextsBlob).
    std::string contexts;
};

/// Open the index as the writer loads it — every manifest adopted, every
/// shard fetched and verified, the header-mode verdicts restored into
/// `commands` — for the commands that walk the whole index or need the
/// build (loaded when `with_build`). Nullopt, with the cause logged, when
/// there is no usable index.
std::optional<LoadedIndex> load_index(Project& project,
                                      CommandResolver& commands,
                                      llvm::StringRef root,
                                      llvm::StringRef requested_configuration,
                                      bool with_build);

/// An inspected file as the index keys it: a relative argument names a
/// file under the workspace, whatever the process working directory, and
/// dot segments are folded the way the compiler's paths were.
std::string inspected_path(const Project& project, llvm::StringRef argument);

}  // namespace clice
