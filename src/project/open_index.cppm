module;

#include "modules/prelude.h"

module clice:project.open_index;

import :config.config;
import :project.command_resolver;
import :project.project;

namespace clice {

/// Open a project's persisted index for reading: the configuration
/// resolved (`requested_configuration` one `config` declares, see
/// check_requested_configuration), the cache store and the blob database
/// opened read-only, the index opened over the database's read snapshot
/// (pinned for the project's lifetime), everything fetched on first use.
/// False — with the cause logged — when there is no usable index.
bool open_index(Project& project, Config config, llvm::StringRef requested_configuration);

/// What load_index found beyond the index: the persisted context choices.
struct LoadedIndex {
    /// The context choices as loaded (see ContextChoices).
    std::string contexts;
};

/// Open the index as the writer loads it — the header-mode results restored
/// into `commands` with it — for the commands that walk the whole index or
/// need the build (loaded when `with_build`). Nullopt, with the cause
/// logged, when there is no usable index.
std::optional<LoadedIndex> load_index(Project& project,
                                      CommandResolver& commands,
                                      Config config,
                                      llvm::StringRef requested_configuration,
                                      bool with_build);

/// What a reader says of an index database the index reports damaged: a
/// writer discards one only when it meets the damage itself, which may sit
/// in records no `clice index` run reads, so the reader names the directory
/// to delete.
std::string damaged_index_error(const Project& project);

/// An inspected file as the command line names it: a relative argument
/// names a file under the workspace, whatever the process working
/// directory.
Spelling inspected_path(const Project& project, llvm::StringRef argument);

}  // namespace clice
