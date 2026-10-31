module clice:project.listings;

import :index.database;
import :index.project_index;
import :support.prelude;
import :vfs.path;

namespace clice {

/// What the index database records of the build (blob kind BuildSnapshot):
/// per source file, the databases that listed it when the index was last
/// saved, and the provisional members. Read once when the project opens: it
/// registers the databases and the provisional members a session would
/// otherwise meet only later, and tells a unit the build stopped compiling
/// from one whose database failed to load (Project::reconcile_index). Which
/// command built a unit's index its manifest records (command_identity).
class SavedListings {
public:
    /// JSON layout of the persisted listings. Paths are the database's names
    /// (ProjectIndex::portable). Listings written before format 53 also hold
    /// each file's command hashes, which the reader ignores.
    struct Listing {
        std::string file;

        /// The databases that listed the file, so a later session can tell a
        /// removal from a database that failed to load.
        std::vector<std::string> sources;
    };

    struct Blob {
        std::vector<Listing> entries;

        /// The files recorded as provisional members, sorted.
        std::vector<std::string> provisional;
    };

    /// Read the listings `database` holds.
    void read(index::BlobDatabase& database);

    /// The listings as read; null when the database held none, or none that
    /// parsed (unreadable).
    const Blob* persisted() const {
        return read_blob ? &*read_blob : nullptr;
    }

    /// The database held listings that did not parse.
    bool unreadable() const {
        return unparsed;
    }

    /// The databases the listings name, as `index` names them on disk: what
    /// discovery registers at startup before anything is opened, so a
    /// nested project's units keep their index across sessions instead of
    /// being dropped as unlisted and rebuilt.
    llvm::SmallVector<Spelling> databases(const index::ProjectIndex& index) const;

    /// The files recorded as provisional members, as `index` names them on
    /// disk.
    std::vector<Spelling> provisional(const index::ProjectIndex& index) const;

    /// The listings need a rewrite no index change will trigger.
    void owe_save() {
        dirty = true;
    }

    /// Whether a save owes the listings a rewrite no index change would
    /// trigger: their last write failed, they were missing or unreadable at
    /// load or recorded other databases than list their files now, or the
    /// provisional members changed (`provisional`, ProvisionalMembers::changes)
    /// since they serialized.
    bool owes_save(std::uint64_t provisional) const {
        return dirty || provisional != serialized_provisional;
    }

    /// `live`, the listings of the build now, for the next save, the
    /// provisional members at `provisional`; nullopt when they serialize as
    /// the persisted ones do, or fail to serialize.
    std::optional<std::string> serialize(const Blob& live, std::uint64_t provisional);

    /// The bytes serialize gave were persisted.
    void saved(std::string bytes) {
        persisted_bytes = std::move(bytes);
        dirty = false;
    }

    /// The bytes serialize gave were not persisted: the next save retries.
    void unsaved() {
        dirty = true;
    }

    /// The database holding the persisted listings is gone: the next save
    /// writes them into its replacement.
    void forget_persisted() {
        read_blob.reset();
        persisted_bytes.clear();
        dirty = true;
    }

private:
    std::optional<Blob> read_blob;

    bool unparsed = false;

    /// The persisted listings' bytes as last read or written; empty when
    /// none exist.
    std::string persisted_bytes;

    /// The persisted listings need a rewrite no index change will trigger:
    /// the load found them missing or corrupt, or found a file listed by
    /// other databases than they record, a batch carrying them did not
    /// commit, or their database was replaced. Without this flag the
    /// rewrite would wait for an unrelated dirtying merge: a save with
    /// nothing else to commit skips the listings entirely.
    bool dirty = false;

    /// ProvisionalMembers::changes as the listings last serialized them: a
    /// provisional member recorded since dirties no blob of its own.
    std::uint64_t serialized_provisional = 0;
};

}  // namespace clice
