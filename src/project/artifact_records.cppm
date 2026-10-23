module;

#include "support/prelude.h"

module clice:project.artifact_records;

import :project.command_resolver;
import :project.project;

namespace clice {

/// The artifact records as the index database carries them (blob kind
/// ArtifactRecords): the PCH and PCM validity metadata and the header-mode
/// results, serialized from live state; empty on serialization failure.
std::string serialize_artifact_records(Project& project, CommandResolver& commands);

/// Restore the artifact records a database held: the PCH and PCM entries
/// whose blobs the cache store still has, and the header modes.
void load_artifact_records(Project& project, CommandResolver& commands, llvm::StringRef bytes);

}  // namespace clice
