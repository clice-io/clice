module;

#include "modules/prelude.h"

module clice:vfs.dir_cache;

namespace clice::vfs {

/// What one readdir found in a directory. Lookups ask a listing instead of
/// calling stat() for each candidate path: dramatically faster on Windows,
/// where individual stat() calls are very expensive (~10x slower than
/// Linux).
struct Listing {
    /// The directory listed, as spelled.
    std::string dir;

    /// Each entry's name, mapped to whether it is a directory (a symlink
    /// counts as what it points to).
    llvm::StringMap<bool> entries;

    /// Each entry's name with ASCII letters lowered, where the filesystem
    /// matches names case-insensitively (Windows, macOS); empty elsewhere.
    llvm::StringSet<> folded;

    /// The directory's mtime the listing provably describes, or 0: the
    /// directory changed during the readdir, the readdir failed or stopped
    /// early, or the mtime lay inside the guard window, where a same-tick
    /// entry creation would not move it. A listing with 0 is trusted for
    /// the operation that took it only.
    std::int64_t mtime_ns = 0;

    /// Whether `name` opens an entry. Windows and macOS match names
    /// case-insensitively, and macOS across Unicode normalizations as well,
    /// but only the volume knows its rule (either can be case-sensitive):
    /// a name the listing holds under another spelling is confirmed with
    /// one stat, as is any non-ASCII name the listing lacks.
    bool contains(llvm::StringRef name) const;

    /// Whether `name` opens an entry other than a directory: what an
    /// #include can name. Matched like contains().
    bool contains_file(llvm::StringRef name) const;
};

/// List `dir`: the pre/post-stat pairing discipline of file reads, on the
/// directory. A missing directory lists as empty. Safe to call from any
/// thread.
std::shared_ptr<const Listing> list(llvm::StringRef dir);

/// Listings kept across operations, validated by the directory's own
/// mtime: POSIX and Windows bump it on entry creation and deletion, so one
/// stat per operation proves a kept listing current — external generators
/// dropping files into include directories produce no event, making the
/// mtime the only anchor there is. Deliberate residual: a forged
/// (backdated) directory mtime defeats this — files have the content hash
/// as a second anchor, a directory's only deeper truth is the readdir
/// itself, and re-reading every use would mean not caching.
struct DirCache {
    /// A new listing replaces the old one, never edits it: a Scope still
    /// holding the old one keeps reading what it validated.
    llvm::StringMap<std::shared_ptr<const Listing>> listings;

    /// The kept listing of `dir` that one stat can revalidate, if any.
    std::shared_ptr<const Listing> kept(llvm::StringRef dir) const;
};

/// The listings one operation (a scan, a rescan, one request) sees: each
/// directory is validated or listed at most once, and the listing holds for
/// the rest of the operation.
struct Scope {
    explicit Scope(DirCache& cache) : cache(cache) {}

    /// The listing of `dir` for this operation.
    const Listing& list(llvm::StringRef dir);

    /// Take a listing `dir` was just listed into (vfs::list, run elsewhere).
    void adopt(llvm::StringRef dir, std::shared_ptr<const Listing> listing);

    DirCache& cache;
    llvm::StringMap<std::shared_ptr<const Listing>> held;

    /// What the operation cost, for the scan report: readdirs made,
    /// listings served without one, names looked up in listings (counted
    /// by the include resolver), and microseconds spent listing.
    struct Stats {
        std::size_t listed = 0;
        std::size_t reused = 0;
        std::size_t lookups = 0;
        std::int64_t us = 0;
    };

    Stats stats;
};

}  // namespace clice::vfs
