#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "command/command.h"
#include "project/project.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

/// Polling of a project's compilation databases: edits by a build system
/// regenerating them, databases appearing after startup, and the response
/// files their commands name. Every reload reports its per-file delta; the
/// caller turns it into invalidation.
///
/// Each tick looks at a source's inputs through the file table and
/// compares their content with what its load read (see
/// CompilationDatabase::inputs), never with a stat taken afterwards: a
/// rewrite landing between the load and the first poll still reads as a
/// change. Whether a stat can stand for the bytes is the file table's
/// call, as for every other file.
class CDBWatcher {
public:
    /// Construct after the project is loaded: every registered source is
    /// baselined at its load.
    CDBWatcher(Project& project, std::string root);

    /// One poll tick. When no rule declares a source, registers every
    /// database discovery finds that is not watched yet, at the root and
    /// its direct subdirectories and above every file of `open_files`
    /// still without a command. Looks at every registered source — declared
    /// ones that do not exist yet included, which is how a database
    /// generated after startup is picked up — and the response files its
    /// commands name. Once a source's inputs have held the same other
    /// content for two consecutive ticks, reloads it. A discovered database
    /// vanishing or returning flips its presence, and the files whose
    /// default entry moves with it change command (see
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
    };

    /// The hashes of the source's last load.
    Hashes loaded(SourceID id) const;

    /// Look at each of the source's inputs on disk now.
    Hashes look(SourceID id);

    /// The file the source's path names now: a symlinked database may have
    /// been pointed elsewhere since its load.
    Fid database(SourceID id);

    /// Register `id` for watching, from its last load.
    void track(SourceID id);

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
