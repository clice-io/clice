module clice:vfs.file_system;

import :support.prelude;

namespace clice {

namespace vfs {

/// What a status says about a file's bytes: two equal stamps of one path
/// describe the same bytes, as far as a status can tell. The change time
/// is what makes that hold for a file replaced under its name with its
/// size and mtime kept (`cp -p`, `rsync -a`): nothing sets it back.
struct Stamp {
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::int64_t ctime_ns = 0;
    fs::FileID id;

    static Stamp of(const fs::Status& status) {
        return {
            .size = status.size,
            .mtime_ns = status.mtime.time_since_epoch().count(),
            .ctime_ns = status.ctime.time_since_epoch().count(),
            .id = status.id,
        };
    }

    friend bool operator==(const Stamp&, const Stamp&) = default;
};

/// Filesystem mtime-granularity guard for freshness baselines: a stat fast
/// path is only recorded for a file whose mtime precedes the reference
/// moment by at least this much. Closer to it, a coarse-granularity
/// filesystem (FAT stores 2s mtimes, several network filesystems whole
/// seconds) could stamp a write the reference never saw with a timestamp
/// from before it — so those files re-earn their fast path through one
/// hash comparison instead.
constexpr inline std::int64_t mtime_guard_ns = 2'000'000'000;

/// The newest mtime (ns) a file may carry and still be provably untouched
/// since the reference moment `at_ms` (a build start, or "now" for
/// content read on the spot).
constexpr std::int64_t stat_baseline_before_ns(std::int64_t at_ms) {
    return at_ms * 1'000'000 - mtime_guard_ns;
}

/// Whether a stat taken now carrying `mtime_ns` may guarantee what it
/// describes beyond this moment.
bool stable_mtime(std::int64_t mtime_ns);

/// A status as clang's file system interface reports it, under `name`.
llvm::vfs::Status to_llvm(const fs::Status& status, llvm::StringRef name);

}  // namespace vfs

/// One observation of a file's on-disk bytes: the xxh3 of the text a single
/// read returned (see without_bom), and the stamp describing the bytes. Captured under
/// the pairing discipline (see vfs::read_observed) so the two halves are
/// same-source: `paired` says the pre/post fstats of the read agreed,
/// `reliable` additionally says the mtime lay outside the filesystem
/// mtime-granularity guard window — only then may the stamp serve as a
/// fast-path baseline for skipping future reads. An unpaired or
/// unreliable observation still carries a true hash of the bytes read.
struct DiskObservation {
    vfs::Stamp stamp;
    std::uint64_t hash = 0;
    bool paired = false;
    bool reliable = false;
};

/// A completed observed read: the observation plus the text it hashed.
struct ObservedFile {
    DiskObservation obs;
    std::string text;
};

namespace vfs {

/// A source file's text as every part of clice sees it: its bytes without
/// a leading UTF-8 byte order mark — the text an editor shows and sends.
/// Clang skips the mark as well, but would count it in every offset.
inline llvm::StringRef without_bom(llvm::StringRef bytes) {
    llvm::StringRef text = bytes;
    text.consume_front("\xEF\xBB\xBF");
    return text;
}

/// A file's text (see without_bom). Read, never mapped: Windows refuses an
/// editor's save over a mapped file.
std::expected<std::string, std::error_code> read_text(llvm::StringRef path);

/// A file's text read and hashed under the pairing discipline: open a
/// handle, fstat it, read through it, fstat again. Equal fstats prove
/// the stat describes the bytes (an in-place write racing the read moves
/// the mtime between the two fstats; a rename-over does not affect the
/// open handle at all). A post-fstat mtime inside the guard window
/// (coarse-granularity filesystems) demotes the pair to unreliable: a
/// racing write can arrive within one mtime tick, so such a stat must not
/// suppress future reads. Safe to call from any thread.
std::expected<ObservedFile, std::error_code> read_observed(llvm::StringRef path);

/// The file statuses of one operation (a dependency check, a compile). On
/// Windows a directory asked about often enough within the operation is
/// listed once, and its entries answer the rest: one listing costs about
/// what a few statuses by name do. A name the listing lacks, a link, a
/// remote or oversized directory and a directory seen to hold hard links
/// take fs::status. Elsewhere every status is fs::status.
class StatusBatch {
public:
    std::expected<fs::Status, std::error_code> status(llvm::StringRef path);

#ifdef _WIN32

private:
    struct Directory {
        unsigned asked = 0;
        bool listed = false;
        /// Empty when the directory could not be listed.
        llvm::StringMap<fs::Status> entries;
    };

    llvm::StringMap<Directory> directories;
#endif
};

/// Warns of a directory fs::walk could not read, unless it is missing.
void warn_unreadable(const fs::Walk::Error& error);

/// Keep the mapping of a PCH from clice's store for the process's later
/// compiles: Windows pages every fresh mapping in fault by fault, on every
/// compile. The store gives each PCH build a name of its own and never
/// rewrites one, so a kept mapping stays the file's bytes. Nothing else
/// qualifies — a PCM is rebuilt under the name it had — since on Windows a
/// held mapping stops anyone from replacing the file.
void keep_mapped(llvm::StringRef path);

/// The files on disk that the compiles of one command do not see, by the
/// search directory holding them.
struct HiddenFiles {
    struct Dir {
        /// A Spelling.
        std::string path;

        /// Relative to `path`, `/`-separated, sorted.
        std::vector<std::string> names;
    };

    std::vector<Dir> dirs;

    /// Whether the absolute `path`, however spelled, names one of them.
    bool hides(llvm::StringRef path) const;
};

/// The file system one compile sees: the disk — sources as their text (see
/// without_bom), `#embed` data, PCH and PCM files as bytes — less the files
/// its command hides. Clang sets its working
/// directory, so every compile gets its own; an overlay wraps a fresh one.
class View : public llvm::vfs::ProxyFileSystem {
public:
    explicit View(HiddenFiles hidden = {}) :
        ProxyFileSystem(llvm::vfs::createPhysicalFileSystem()), hidden(std::move(hidden)) {}

    /// The size of the text: clang checks what it reads against it, and a
    /// PCH records it for every input. Status carries no use, so a binary
    /// use of a file clang only stat'ed first gets the text: `#embed` after
    /// `__has_embed` probed the same file reads it without the mark.
    llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine& path) override;

    /// Named by the path the OS opened (its RealName), which names include
    /// results and drives `-Wnonportable-include-path`.
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
        openFileForRead(const llvm::Twine& path) override;

    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
        openFileForReadBinary(const llvm::Twine& path) override;

private:
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> open(const llvm::Twine& path, bool text);

private:
    StatusBatch statuses;

    HiddenFiles hidden;
};

}  // namespace vfs

}  // namespace clice
