module clice:project.saved;

import :index.database;
import :index.project_index;
import :support.prelude;
import :vfs.file_table;
import :vfs.path;

namespace clice {

/// The table of paths a persisted record names its files by, `paths`: each
/// portable across checkouts (ProjectIndex::portable) and written once.
class PathTable {
public:
    PathTable(const index::ProjectIndex& index, FileTable& files, std::vector<std::string>& paths) :
        index(index), files(files), paths(paths) {}

    /// The index of the file's path, added at its first use.
    std::uint32_t add(Fid file);

    /// The file at `at`; nullopt past the table or for an empty path.
    std::optional<Fid> file(std::uint32_t at) const;

private:
    const index::ProjectIndex& index;

    FileTable& files;

    std::vector<std::string>& paths;

    llvm::StringMap<std::uint32_t> indices;
};

/// What the index database records of the build's members (blob kind
/// Members): per source file, the databases that listed it when the index
/// was last saved, and the provisional members. Read once when the project
/// opens: it registers the databases and the provisional members a session
/// would otherwise meet only later, and tells a unit the build stopped
/// compiling from one whose database failed to load
/// (Project::reconcile_index). Which command built a unit's index its
/// manifest records (command_identity).
class SavedMembers {
public:
    /// JSON layout of the persisted members. Paths are the database's names
    /// (ProjectIndex::portable). Records written before format 53 also hold
    /// each file's command hashes, which the reader ignores.
    struct Member {
        std::string file;

        /// The databases that listed the file, so a later session can tell a
        /// removal from a database that failed to load.
        std::vector<std::string> sources;
    };

    struct Blob {
        std::vector<Member> entries;

        /// The files recorded as provisional members, sorted.
        std::vector<std::string> provisional;
    };

    /// Read the record `database` holds.
    void read(index::BlobDatabase& database);

    /// The record as read; null when the database held none, or none that
    /// parsed (unreadable).
    const Blob* persisted() const {
        return read_blob ? &*read_blob : nullptr;
    }

    /// The database held a record that did not parse.
    bool unreadable() const {
        return unparsed;
    }

    /// The databases the record names, as `index` names them on disk: what
    /// discovery registers at startup before anything is opened, so a
    /// nested project's units keep their index across sessions instead of
    /// being dropped as unlisted and rebuilt.
    llvm::SmallVector<Spelling> databases(const index::ProjectIndex& index) const;

    /// The files recorded as provisional members, as `index` names them on
    /// disk.
    std::vector<Spelling> provisional(const index::ProjectIndex& index) const;

    /// The record needs a rewrite no index change will trigger.
    void owe_save() {
        dirty = true;
    }

    /// Whether a save owes the record a rewrite no index change would
    /// trigger: its last write failed, it was missing or unreadable at load
    /// or recorded other databases than list their files now, or the
    /// provisional members changed (`provisional`, ProvisionalMembers::changes)
    /// since it serialized.
    bool owes_save(std::uint64_t provisional) const {
        return dirty || provisional != serialized_provisional;
    }

    /// `live`, the members of the build now, for the next save, the
    /// provisional members at `provisional`; nullopt when they serialize as
    /// the persisted ones do, or fail to serialize.
    std::optional<std::string> serialize(const Blob& live, std::uint64_t provisional);

    /// The bytes serialize gave were persisted.
    void saved(std::string bytes) {
        persisted_bytes = std::move(bytes);
        dirty = false;
    }

    /// The database holding the persisted record is gone: the next save
    /// writes it into its replacement.
    void forget_persisted() {
        read_blob.reset();
        persisted_bytes.clear();
        dirty = true;
    }

private:
    std::optional<Blob> read_blob;

    bool unparsed = false;

    /// The persisted record's bytes as last read or written; empty when
    /// none exists.
    std::string persisted_bytes;

    /// The persisted record needs a rewrite no index change will trigger:
    /// the load found it missing or corrupt, or found a file listed by
    /// other databases than it records, a batch carrying it did not
    /// commit, or their database was replaced. Without this flag the
    /// rewrite would wait for an unrelated dirtying merge: a save with
    /// nothing else to commit skips the record entirely.
    bool dirty = false;

    /// ProvisionalMembers::changes as the record last serialized them: a
    /// provisional member recorded since dirties no blob of its own.
    std::uint64_t serialized_provisional = 0;
};

}  // namespace clice
