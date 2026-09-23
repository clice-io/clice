#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "command/command.h"
#include "project/project.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

/// Stat-polling of a project's compilation databases: edits by a build
/// system regenerating them, databases appearing after startup, and the
/// response files their commands name. Every reload reports its per-file
/// delta; the caller turns it into invalidation.
///
/// A database's baseline is the read its loaded entries came from (see
/// CompilationDatabase::observation), not a stat taken afterwards: a
/// rewrite landing between the load and the first poll must still read as
/// a change. A baseline whose stat cannot vouch for the bytes — written
/// within the filesystem's mtime granularity of the read — is compared by
/// content until it can, so a same-size rewrite in the same mtime tick is
/// not invisible. An mtime ahead of the clock (skew on a network
/// filesystem) cannot vouch either: such a database is compared by content
/// on every poll until the clock catches up.
class CDBWatcher {
public:
    /// Construct after the project is loaded: every registered source is
    /// baselined at its load.
    CDBWatcher(Project& project, std::string root);

    /// One poll tick. When no rule declares a source, registers every
    /// database discovery finds that is not watched yet, at the root and
    /// its direct subdirectories and above every file of `open_files`
    /// still without a command. Stats every registered source — declared
    /// ones that do not exist yet included, which is how a database
    /// generated after startup is picked up — and the response files its
    /// commands name. Once a source's stamp change has stayed stable for
    /// two consecutive ticks, reloads it. A discovered database vanishing
    /// or returning flips its presence, and the files whose default entry
    /// moves with it change command (see Build::source_order); its entries
    /// keep serving meanwhile.
    ///
    /// `force` reloads unconditionally: it skips both the stamp gate and
    /// the two-tick settling debounce (the half-written-file guard). The
    /// test hook uses it so a single poll request applies a change
    /// deterministically; a spurious forced reload just yields an empty
    /// diff.
    CDBDiff tick(llvm::ArrayRef<Fid> open_files, bool force = false);

    /// Register, load and watch the databases in the directories from the
    /// file's up to the root, which startup discovery (the root and its
    /// direct subdirectories) did not look at: a file of a deeper project
    /// compiles from its own database. Nothing when a rule declares
    /// sources, the file has a command already, or it lies outside the
    /// root.
    CDBDiff discover_around(Fid path_id);

private:
    /// (existence, size, mtime, filesystem identity) of a file: a
    /// rename-over with a forged equal size and mtime still changes the
    /// UniqueID.
    struct FileStamp {
        bool exists = false;
        std::uint64_t size = 0;
        std::int64_t mtime_ns = 0;
        std::uint64_t uid_device = 0;
        std::uint64_t uid_file = 0;

        friend bool operator==(const FileStamp&, const FileStamp&) = default;
    };

    static FileStamp stat_file(llvm::StringRef path);

    static FileStamp stamp_of(const DiskObservation& observed);

    /// The stamp of a source: its database and the response files its
    /// commands name.
    struct SourceStamp {
        FileStamp database;
        llvm::SmallVector<FileStamp> responses;

        friend bool operator==(const SourceStamp&, const SourceStamp&) = default;
    };

    SourceStamp stat_source(SourceID id) const;

    /// One registered source's watch state.
    struct TrackedSource {
        SourceID id;
        /// The stamp the loaded entries correspond to.
        SourceStamp applied;
        /// Hash of the database bytes the loaded entries came from.
        std::uint64_t hash = 0;
        /// Whether `applied.database` vouches for those bytes: taken
        /// outside the mtime-granularity window of the read. Until it
        /// does, an unchanged stamp is confirmed by content.
        bool trusted = true;
        /// Debounce: the stamp observed on the previous tick, not yet settled.
        SourceStamp pending;
        bool has_pending = false;
        /// The last load (the startup one included) first named response
        /// files, whose stamps could only be taken after it read them:
        /// reload once more, so a rewrite landing in between is not
        /// missed.
        bool reread = false;
    };

    /// Register `id` for watching, baselined at its load.
    void track(SourceID id);

    /// Adopt the source's last load as its baseline.
    void adopt_load(TrackedSource& tracked);

    /// Whether the database still holds the bytes its entries came from,
    /// its stamp unchanged; a match that the stat can now vouch for
    /// becomes the trusted baseline.
    bool content_unchanged(TrackedSource& tracked);

    /// Tick one source, its reload's delta merged into `delta`.
    void tick_source(TrackedSource& tracked, bool force, CDBDiff& delta);

    /// discover_around's work, its delta merged into `delta`.
    void discover_into(Fid path_id, CDBDiff& delta);

    /// The files the source and another one both list: the ones whose
    /// default entry may move with the source's presence.
    llvm::SmallVector<Fid> shared_files(SourceID id) const;

    /// The source of each file's default entry, as the build ranks them
    /// now; none for a file the build no longer compiles.
    llvm::SmallVector<std::optional<SourceID>> default_sources(llvm::ArrayRef<Fid> files) const;

    Project& project;
    std::string root;

    llvm::SmallVector<TrackedSource> sources;
};

}  // namespace clice
