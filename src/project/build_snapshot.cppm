module clice:project.build_snapshot;

import :index.project_index;
import :support.prelude;
import :vfs.ids;
import :vfs.path;

namespace clice {

struct Project;

/// What the index database records of the build (blob kind BuildSnapshot):
/// per source file, the databases that listed it when the index was last
/// saved, and the provisional members. Which command built a unit's index
/// its manifest records (command_identity); the snapshot is what tells a
/// unit the build stopped compiling from one whose database failed to
/// load, and what a later session registers before anything is opened.
class BuildSnapshot {
public:
    /// What the persisted snapshot remembers of the build, as absolute
    /// paths.
    struct Remembered {
        /// The databases: what discovery registers at startup before
        /// anything is opened, so a nested project's units keep their index
        /// across sessions instead of being dropped as unlisted and rebuilt.
        llvm::SmallVector<Spelling> sources;

        /// The files recorded as provisional members (Build::recorded).
        std::vector<Spelling> provisional;
    };

    /// The build the persisted snapshot of `project`'s database remembers.
    static Remembered remembered(const Project& project);

    /// Bring the loaded index in line with the live build: every unit whose
    /// command changed while no server ran is owed a reindex in `report`,
    /// its index serving until the reindex merges. A unit the build stopped
    /// compiling — every database that listed it is healthy and lists it no
    /// more, or the default command that covered it is gone — leaves the
    /// index; one a database that failed to load listed keeps serving its
    /// last-known index, mirroring the live CDB-reload treatment.
    void reconcile(Project& project, index::IndexReport& report);

    /// Whether a save owes the snapshot a rewrite no index change would
    /// trigger: its last write failed, it was missing or unreadable at load
    /// or recorded other databases than list its files now, or a
    /// provisional member was recorded since it serialized.
    bool owes_save(const Project& project) const;

    /// The snapshot of the live build for the next save; nullopt when it
    /// serializes as the persisted one does, or fails to serialize.
    std::optional<std::string> serialize(Project& project);

    /// The bytes serialize gave were persisted.
    void saved(std::string bytes) {
        persisted_bytes = std::move(bytes);
        dirty = false;
    }

    /// The bytes serialize gave were not persisted: the next save retries.
    void unsaved() {
        dirty = true;
    }

    /// The database holding the persisted snapshot is gone: the next save
    /// writes one into its replacement.
    void forget_persisted() {
        persisted_bytes.clear();
        dirty = true;
    }

private:
    /// The persisted snapshot's bytes as last read or written; empty when
    /// none exists.
    std::string persisted_bytes;

    /// The persisted snapshot needs a rewrite no index change will
    /// trigger: the load found it missing or corrupt, or found a file
    /// listed by other databases than it records, a batch carrying it did
    /// not commit, or its database was replaced. Without this flag the
    /// rewrite would wait for an unrelated dirtying merge: a save with
    /// nothing else to commit skips the snapshot entirely.
    bool dirty = false;

    /// Build::recorded_generation as the snapshot last serialized it: a
    /// provisional member recorded since dirties no blob of its own.
    std::uint64_t serialized_records = 0;
};

}  // namespace clice
