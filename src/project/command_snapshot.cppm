module clice:project.command_snapshot;

import :index.project_index;
import :support.prelude;
import :vfs.ids;
import :vfs.path;

namespace clice {

struct Project;

/// What the index database records of the commands the index was built
/// with (blob kind CommandSnapshot): per source file, the sorted canonical
/// command hashes of its entries and a hash of its matched config rules when
/// the index was last saved; per unit without entries, the command covering
/// it and its matched rules. A load diffs it against the live build to drop
/// the index of every TU whose command changed while no server ran —
/// content-based freshness cannot see command changes — and the next save
/// rewrites it when the live build serializes differently.
class CommandSnapshot {
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

    /// Diff the persisted snapshot against the live build and drop the
    /// index of every TU whose compile command changed while no server was
    /// running, with its reindex owed in `report`. A TU the build stopped
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

    /// The snapshot of the live build for the next save, with the units
    /// without entries among `debt` recorded as owed; nullopt when it
    /// serializes as the persisted one does, or fails to serialize.
    std::optional<std::string> serialize(Project& project, llvm::ArrayRef<Fid> debt);

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
