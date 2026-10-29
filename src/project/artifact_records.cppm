module clice:project.artifact_records;

import :project.project;
import :support.prelude;

namespace clice {

/// The artifact records as the index database carries them (blob kind
/// ArtifactRecords): the PCH and PCM validity metadata and the
/// self-containment trials, serialized from live state; empty on
/// serialization failure.
std::string serialize_artifact_records(Project& project);

/// Restore the artifact records a database held: the PCH and PCM entries
/// whose blobs the cache store still has, and the trials.
void load_artifact_records(Project& project, llvm::StringRef bytes);

}  // namespace clice
