module;

#include "modules/prelude.h"

module clice:project.cdb_watcher;

import :command.command;
import :project.project;

namespace clice {

/// Polling of a project's compilation databases: edits by a build system
/// regenerating them, databases appearing after startup, and the response
/// files their commands name. Every reload reports its per-file delta; the
/// caller turns it into invalidation.
///
/// A source's inputs, and the places a database may appear, are flags the
/// file table looks at (see vfs::DiskState::watch); a tick here only weighs
/// what those looks found. A source's inputs are compared with what its
/// load read (see CompilationDatabase::inputs), never with a look taken
/// afterwards: a rewrite landing between the load and the first look still
/// reads as a change.
class CDBWatcher {
public:
    /// Construct after the project is loaded: every registered source is
    /// baselined at its load.
    CDBWatcher(Project& project, CanonicalPath root);

    /// One tick, after the file table looked at the flags. When no rule
    /// declares a source, a database found at a watched place is registered:
    /// the root and its direct subdirectories (listed again when the root
    /// directory moves), and every directory from a file of `open_files`
    /// still without a command up to the root — which is how a database
    /// generated after startup is picked up. Declared sources are
    /// registered whether they exist or not. Once a source's inputs have
    /// held the same other content for two consecutive ticks, reloads it. A
    /// discovered database vanishing or returning flips its presence, and
    /// the files whose default entry moves with it change command (see
    /// Build::source_order); its entries keep serving meanwhile.
    ///
    /// `force` reloads unconditionally: it skips both the content gate and
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
    /// What the disk holds for each of a source's inputs, in
    /// CompilationDatabase::inputs order: the content hash, nullopt when
    /// there are no bytes to read.
    using Hashes = llvm::SmallVector<std::optional<std::uint64_t>>;

    /// One registered source's watch state.
    struct TrackedSource {
        SourceID id;
        /// What the watcher last settled on: the reads of the source's last
        /// load, or a deletion it acted on.
        Hashes applied;
        /// Half-write guard: what the previous tick saw while it differed
        /// from `applied`; a tick seeing it again reloads.
        std::optional<Hashes> pending;
        /// The source's inputs, in CompilationDatabase::inputs order. The
        /// database is watched by the path the source names: a symlinked
        /// one may be pointed elsewhere since its load.
        llvm::SmallVector<std::shared_ptr<const vfs::Flag>, 0> inputs;
    };

    /// The hashes of the source's last load.
    Hashes loaded(SourceID id) const;

    /// What the last looks found of each of the source's inputs.
    static Hashes looked(const TrackedSource& tracked);

    /// Watch the response files the source's last load read, besides the
    /// database.
    void watch_inputs(TrackedSource& tracked);

    /// Register `id` for watching, from its last load.
    void track(SourceID id);

    /// Register and watch the databases found at the watched places,
    /// watching the places `open_files` need from now on.
    void discover(llvm::ArrayRef<Fid> open_files);

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
    CanonicalPath root;

    llvm::SmallVector<TrackedSource> sources;

    /// The root directory, whose stamp moves when an entry appears or goes.
    std::shared_ptr<const vfs::Flag> root_flag;
    /// The root's stamp at the listing `listed` came from; nullopt while no
    /// listing can be trusted to hold.
    std::optional<vfs::Stamp> listed_at;
    /// The places database_places names under the root.
    llvm::SmallVector<Spelling> listed;
    /// The watched places with no source registered there yet.
    llvm::StringMap<std::shared_ptr<const vfs::Flag>> places;
    /// The places a registered source is known to stand at.
    llvm::StringSet<> registered;
};

}  // namespace clice
