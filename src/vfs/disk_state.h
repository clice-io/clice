#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "vfs/file_system.h"
#include "vfs/ids.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice::vfs {

/// What the master knows of the files on disk: per file, what the last
/// look through its fid found, and the changes those looks saw. A look is
/// a status of the file and, unless an earlier read vouches for that
/// status, a read. Whoever looks — a freshness check, a rescan, a save,
/// the workspace sweep — records it here, so this is the single source of
/// disk change events.
///
/// Every spelling of a file shares its fid (see FileTable::intern), while
/// hardlinks are distinct fids, each with its own reads: nothing here is
/// shared between fids.
class DiskState {
public:
    /// `paths` names each fid's file, indexed by its raw value.
    explicit DiskState(const llvm::SmallVectorImpl<llvm::StringRef>& paths) : paths(paths) {}

    DiskState(const DiskState&) = delete;
    DiskState& operator=(const DiskState&) = delete;

    /// The disk content as last seen through this fid, without I/O;
    /// nullopt before the first look and while the file is missing.
    std::optional<std::uint64_t> seen_hash(Fid fid) const;

    /// Whether the last look through this fid found the file missing.
    bool seen_missing(Fid fid) const;

    /// Every fid the last look found missing: deleted files, and the places
    /// failed lookups looked — where a file appearing is a change.
    llvm::SmallVector<Fid> missing_files() const;

    /// The changed files, in first-change order, emptying the queue.
    llvm::SmallVector<Fid> take_changes();

    /// Invoked when the change queue goes from empty to non-empty; the
    /// owner schedules the drain. Unset (batch tools, tests) leaves the
    /// queue to whoever takes it.
    std::function<void()> on_change;

    /// A look found the file missing.
    void saw_missing(Fid fid);

    /// Record a same-source read (the scan worker's, or one made through
    /// read()). Unpaired or unreliable reads carry a true hash but no
    /// proof for their stamp, so they never vouch for a later status.
    void observe(Fid fid, const DiskObservation& obs);

    /// Read the file under the pairing discipline and record it. nullopt =
    /// unreadable right now (what was seen is left untouched; what a failed
    /// read means is the caller's policy).
    std::optional<DiskObservation> read(Fid fid);

    /// Stat the file and produce a same-source observation of its current
    /// content. nullopt = missing or unreadable.
    std::optional<DiskObservation> current(Fid fid);

    /// A same-source observation for a status the caller just took: the
    /// hash the last reliable read vouches for when its stamp equals the
    /// status's, else a real read (whose observation may describe a newer
    /// stamp than the caller's, which is then simply newer truth). nullopt
    /// = unreadable right now.
    std::optional<DiskObservation> observe_for(Fid fid, const Status& status);

    /// The hash the last reliable read through this fid vouches for at
    /// exactly this stamp, recorded as a look; nullopt when someone must
    /// read. Equality, never a watermark: the hash is "the hash of the
    /// bytes that had this stamp", nothing else.
    std::optional<std::uint64_t> cached_hash(Fid fid, const Stamp& stamp);

    /// How a check of a file's content came out. Policy-free facts; what
    /// Missing or Unreadable *means* differs per consumer and stays with
    /// the caller.
    enum class Verdict : std::uint8_t {
        /// The disk provably holds the bytes.
        Fresh,
        /// The disk holds different bytes.
        Stale,
        /// The file does not exist now.
        Missing,
        /// The file exists but cannot be read right now.
        Unreadable,
    };

    /// RAII scope of one check operation (a deps_changed chain, an index
    /// need_update batch): every file is looked at at most once inside
    /// it, so a memo of one operation can never leak into the next. Waves
    /// do not nest, and a wave must not span a suspension point — a save
    /// landing mid-wave would leave memoized looks describing the old
    /// disk.
    class [[nodiscard]] Wave {
    public:
        explicit Wave(DiskState& state);
        ~Wave();

        Wave(const Wave&) = delete;
        Wave& operator=(const Wave&) = delete;

    private:
        DiskState& state;
    };

    Wave wave() {
        return Wave(*this);
    }

    /// Whether the disk still holds the bytes hashing to `hash`. Hash 0 is
    /// the consumed-hash sentinel for "the worker had no bytes to hash":
    /// nothing to compare against, never fresh.
    Verdict check(Fid fid, std::uint64_t hash);

    /// Whether a file is there and readable now, for a place a build found
    /// empty.
    bool present(Fid fid);

private:
    /// The last reliable read: the hash of the bytes the stamp described.
    struct Pair {
        Stamp stamp;
        std::uint64_t hash = 0;
    };

    struct File {
        /// The content hash, or nullopt when the file was missing.
        std::optional<std::uint64_t> seen;
        std::optional<Pair> pair;
    };

    /// What a wave's look at a file found.
    struct Look {
        enum Kind : std::uint8_t { Missing, Unreadable, Read } kind;

        std::uint64_t hash = 0;
    };

    llvm::StringRef path(Fid fid) const {
        return paths[fid.raw];
    }

    /// Record a look's finding: a first look is no change — nothing was
    /// derived from an unseen state; any other finding than the last one
    /// is.
    void saw(Fid fid, std::optional<std::uint64_t> hash);

    /// The wave's look at a file, taken once per wave.
    Look look(Fid fid);

    const llvm::SmallVectorImpl<llvm::StringRef>& paths;

    /// No entry before the first look.
    llvm::DenseMap<Fid, File> files;

    llvm::SmallVector<Fid> changes;
    llvm::DenseSet<Fid> changed;

    llvm::DenseMap<Fid, Look> wave_looks;
    StatusBatch wave_statuses;
    bool wave_open = false;
};

}  // namespace clice::vfs
