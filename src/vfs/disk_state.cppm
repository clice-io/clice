module;

#include "modules/prelude.h"

module clice:vfs.disk_state;

import :vfs.file_system;
import :vfs.ids;

namespace clice::vfs {

/// A file watched by its path, a symlink followed anew at each look: the
/// markers of a git checkout, a package environment, a compilation
/// database. A directory has no content: its stamp alone moves.
struct Flag {
    std::string path;
    /// The content hash at the last look; nullopt while the file is
    /// missing or unreadable.
    std::optional<std::uint64_t> hash;
    /// The stamp at the last look; nullopt while the file is missing.
    std::optional<Stamp> stamp;
    /// The stamp a reliable read of `hash` was taken under.
    std::optional<Stamp> hashed;

    /// Look again; whether the stamp or the content moved. A marker can
    /// move without its content: pixi rewrites an environment's history
    /// with the same line at every install.
    bool look();
};

/// What the master knows of the files on disk: per file, what the last
/// look through its fid found, and the changes those looks saw. A look is
/// a status of the file and, unless an earlier read vouches for that
/// status, a read. Whoever looks — a freshness check, a rescan, a save, a
/// background tick — records it here, so this is the single source of disk
/// change events.
///
/// Every spelling of a file shares its fid (see FileTable::intern), while
/// hardlinks are distinct fids, each with its own reads: nothing here is
/// shared between fids.
///
/// Each file looked at is also due for its next look after an interval
/// that tick() keeps: its root's policy's shortest, doubled at every look
/// that finds the file unchanged and dropped back at a change. A
/// check of a workspace file always looks; a check of a package file is
/// answered "unchanged" without a look when the last look, not yet due,
/// found what the check expects — any other answer is the disk's. No tick
/// running lets every file fall due, so every check looks again.
///
/// A check looks at a file once per turn of the owner's event loop: the
/// steps of one request — a staleness check, the PCH it prepares, the
/// build it starts — run in one turn and share the look. Any other look at
/// the file replaces what the turn remembers of it, so a turn never answers
/// against what the master saw since; it only misses a change nobody looked
/// at yet, like the window between a check and the compile after it.
class DiskState {
public:
    using Clock = std::chrono::steady_clock;

    /// Who changes a file, which decides how a check answers for it.
    enum class Kind : std::uint8_t {
        /// The user, and the tools they run — sources, generated files,
        /// anything under no registered root, edited by agents outside the
        /// editor as often as in it: a check always looks.
        Workspace,
        /// An install or upgrade, which replaces a toolchain or an
        /// environment as a whole: a check trusts a look that is not due.
        Package,
    };

    /// How often the files of a root are looked at.
    struct Policy {
        Kind kind = Kind::Workspace;
        Clock::duration min = std::chrono::seconds(1);
        /// Zero: never looked at in the background.
        Clock::duration max = std::chrono::seconds(30);
    };

    const static Policy workspace_policy;
    const static Policy package_policy;

    /// `paths` names each fid's file, indexed by its raw value.
    explicit DiskState(const llvm::SmallVectorImpl<llvm::StringRef>& paths) : paths(paths) {}

    DiskState(const DiskState&) = delete;
    DiskState& operator=(const DiskState&) = delete;

    /// The disk content as last seen through this fid, without I/O;
    /// nullopt before the first look and while the file is missing.
    std::optional<std::uint64_t> seen_hash(Fid fid) const;

    /// Whether the last look through this fid found the file missing.
    bool seen_missing(Fid fid) const;

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
    /// unreadable right now (what was seen is left untouched, though no
    /// longer trusted; what a failed read means is the caller's policy).
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

    /// Invoked when a check opens a turn, for the owner to end it
    /// (end_turn). Unset, every check is a turn of its own.
    std::function<void()> on_turn;

    /// Forget the turn's looks: the next check looks again.
    void end_turn();

    /// End every turn right before `loop` next waits for IO, until the
    /// task is cancelled: the event loop's owner runs it.
    kota::task<> end_turns(kota::event_loop& loop);

    /// Whether the disk still holds the bytes hashing to `hash`. Hash 0 is
    /// the consumed-hash sentinel for "the worker had no bytes to hash":
    /// nothing to compare against, never fresh.
    Verdict check(Fid fid, std::uint64_t hash);

    /// Whether a file is there and readable now, for a place a build found
    /// empty.
    bool present(Fid fid);

    /// Files under the directory `dir` (an identity) follow `policy`, the
    /// deepest root deciding; files under none follow workspace_policy.
    void add_root(llvm::StringRef dir, Policy policy);

    /// A directory a toolchain installed (an identity): its files follow
    /// package_policy, and when it lies in a conda environment (pixi's
    /// included), the environment's install history is watched — an
    /// install or upgrade makes everything in the environment due.
    void add_package(llvm::StringRef dir);

    /// A build read this file's bytes, hashing to `hash`: for a file nobody
    /// looked at yet, that read is the first look, so that a change after
    /// it is a change.
    void consumed(Fid fid, std::uint64_t hash);

    /// Make every file under the directory `dir` (an identity) due now.
    void expire_under(llvm::StringRef dir);

    /// Look at every watched flag, then at the files that are due, longest
    /// due first, for about `budget` of wall time.
    void tick(Clock::duration budget);

    /// Look at these files now, due or not, under any policy.
    void look(llvm::ArrayRef<Fid> fids);

    /// Record which of these files are missing now, a status each: a file
    /// that is there is left as it was, unread.
    void find_missing(llvm::ArrayRef<Fid> fids);

    /// Look at every watched flag and every file now: the test hook's
    /// deterministic stand-in for the ticks.
    void look_all();

    /// Look at every watched flag now, as a tick does first.
    void look_flags();

    /// Look at `path` at every tick from now on, until the returned flag is
    /// dropped, calling `on_change`, when given, at a look that finds other
    /// content than the one before. Its first look is taken now.
    std::shared_ptr<const Flag> watch(std::string path, std::function<void()> on_change = {});

    /// The time of the schedule; tests turn it.
    std::function<Clock::time_point()> now = Clock::now;

    /// Check every answer a check gives without looking against a look,
    /// reporting a contradiction as an anomaly: the test suites run with it.
    bool shadow = false;

    /// How the turns' checks were answered: by a look at the disk, or from
    /// a look not yet due.
    struct Checks {
        std::uint64_t looked = 0;
        std::uint64_t trusted = 0;
    } checks;

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
        /// The root whose policy the file follows; no_root for none.
        std::uint32_t root = no_root;
        Clock::duration interval{};
        Clock::time_point due;
        /// When the file's entry in the queue comes up; max() when it has
        /// none.
        Clock::time_point queued = Clock::time_point::max();
    };

    constexpr static std::uint32_t no_root = ~0u;

    struct Root {
        std::string dir;
        Policy policy;
    };

    struct Due {
        Clock::time_point at;
        Fid fid;
    };

    /// What a turn's look at a file found.
    struct Look {
        enum class Found : std::uint8_t { Missing, Unreadable, Read } found;

        std::uint64_t hash = 0;

        friend bool operator==(const Look&, const Look&) = default;
    };

    llvm::StringRef path(Fid fid) const {
        return paths[fid.raw];
    }

    /// Record a look's finding: a first look is no change — nothing was
    /// derived from an unseen state; any other finding than the last one
    /// is. The file is due again after its interval: the shortest after a
    /// first look, a change, or an mtime not yet `settled` (see
    /// vfs::settled), else twice the last one.
    void saw(Fid fid, std::optional<std::uint64_t> hash, bool settled);

    /// The turn's look at a file, taken once per turn — unless the last
    /// look, at a package file not yet due, found what is `expected`.
    Look turn_look(Fid fid, std::optional<Look> expected);

    /// The last look's finding, for a package file not yet due.
    std::optional<Look> trusted(Fid fid);

    /// Report a trusted finding a look contradicts.
    void verify(Fid fid, const Look& found);

    const Policy& policy(const File& file) const {
        return file.root == no_root ? workspace_policy : roots[file.root].policy;
    }

    std::uint32_t root_of(llvm::StringRef path) const;

    /// Make the file due at `at`, queueing it.
    void schedule(Fid fid, File& file, Clock::time_point at);

    /// Bring the file up at `at`, unless an earlier entry already will.
    void enqueue(Fid fid, File& file, Clock::time_point at);

    /// A background look at a file.
    void look_at(Fid fid, StatusBatch& statuses);

    struct Watch {
        Flag flag;
        std::function<void()> on_change;
    };

    const llvm::SmallVectorImpl<llvm::StringRef>& paths;

    /// No entry before the first look.
    llvm::DenseMap<Fid, File> files;

    llvm::SmallVector<Fid> changes;
    llvm::DenseSet<Fid> changed;

    llvm::DenseMap<Fid, Look> turn_looks;
    StatusBatch turn_statuses;
    bool turn_open = false;

    llvm::SmallVector<Root> roots;

    /// A min-heap on `at`. An entry whose time is not its file's `queued`
    /// is a leftover of a rescheduling, skipped when it comes up.
    llvm::SmallVector<Due> queue;

    llvm::SmallVector<std::weak_ptr<Watch>> watches;

    /// The install history watched for each conda environment.
    llvm::StringMap<std::shared_ptr<const Flag>> environments;
};

const inline DiskState::Policy DiskState::workspace_policy{};

const inline DiskState::Policy DiskState::package_policy{
    .kind = Kind::Package,
    .min = std::chrono::seconds(30),
    .max = std::chrono::minutes(10),
};

}  // namespace clice::vfs
