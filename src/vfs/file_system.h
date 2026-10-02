#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"

namespace clice {

namespace vfs {

/// What a status says about a file's bytes: two equal stamps of one path
/// describe the same bytes, as far as a status can tell. The change time
/// is what makes that hold for a file replaced under its name with its
/// size and mtime kept (`cp -p`, `rsync -a`): nothing sets it back.
struct Stamp {
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    /// The inode change time; ChangeTime on Windows.
    std::int64_t ctime_ns = 0;
    /// The file's ID: the device, and the file on it.
    std::uint64_t device = 0;
    std::uint64_t file = 0;

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

/// Whether a stat taken now carrying `mtime_ns` may vouch for what it
/// describes beyond this moment.
bool settled(std::int64_t mtime_ns);

/// A file's status, symlinks followed.
struct Status {
    llvm::sys::fs::file_type type = llvm::sys::fs::file_type::status_error;
    Stamp stamp;
    std::uint32_t links = 0;

    bool is_file() const {
        return type == llvm::sys::fs::file_type::regular_file;
    }

    /// As clang's file system interface reports it.
    llvm::vfs::Status to_llvm(llvm::StringRef name) const;
};

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
    std::unique_ptr<llvm::MemoryBuffer> content;
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

/// How a read serves a file.
enum class Read : std::uint8_t {
    /// Its text (see without_bom), copied into memory: sources, commands
    /// and configuration. Never mapped — Windows refuses an editor's save
    /// over a mapped file.
    Text,
    /// Its bytes as they are, copied into memory.
    Bytes,
    /// Its bytes as they are, mapped when large: build artifacts clice
    /// reads but never edits.
    Mapped,
};

/// A file, read whole.
std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::error_code> read(llvm::StringRef path,
                                                                         Read as = Read::Text);

/// A file's text read and hashed under the pairing discipline: open a
/// handle, fstat it, read through it, fstat again. Equal fstats prove
/// the stat describes the bytes (an in-place write racing the read moves
/// the mtime between the two fstats; a rename-over does not affect the
/// open handle at all). A post-fstat mtime inside the guard window
/// (coarse-granularity filesystems) demotes the pair to unreliable: a
/// racing write can land within one mtime tick, so such a stat must not
/// suppress future reads. Safe to call from any thread.
std::expected<ObservedFile, std::error_code> read_observed(llvm::StringRef path);

/// A file's status, symlinks followed.
///
/// Windows makes a status expensive by opening the file (every filter
/// driver sees the open), so there it is taken by name without an open
/// where the system can (Windows 11 24H2 on). A file's ID then comes from
/// the volume's file ID instead of LLVM's hash of the file's final path,
/// and View reports the same IDs for the files it opens: clang tells files
/// apart by ID, and one scheme per process keeps one file one file. Network
/// volumes keep LLVM's scheme, their file IDs can be reused.
std::expected<Status, std::error_code> status(llvm::StringRef path);

/// The file statuses of one operation (a dependency check, a compile). On
/// Windows a directory asked about often enough within the operation is
/// listed once, and its entries answer the rest: one listing costs about
/// what a few statuses by name do. A name the listing lacks, a reparse
/// point, a remote or oversized directory and a directory seen to hold
/// hard links take vfs::status. Elsewhere every status is vfs::status.
class StatusBatch {
public:
    std::expected<Status, std::error_code> status(llvm::StringRef path);

#ifdef _WIN32

private:
    struct Directory {
        unsigned asked = 0;
        bool listed = false;
        /// Empty when the directory could not be listed.
        llvm::StringMap<Status> entries;
    };

    llvm::StringMap<Directory> directories;
#endif
};

/// Whether a path names anything, symlinks followed.
bool exists(llvm::StringRef path);

/// Whether a path names a regular file, symlinks followed.
bool is_file(llvm::StringRef path);

/// Whether a path names a directory, symlinks followed.
bool is_directory(llvm::StringRef path);

/// Whether a path is itself a symlink; never on Windows (see Entry).
bool is_symlink(llvm::StringRef path);

/// An entry of a directory: its path, the directory's spelling joined with
/// its name, and its type, a symlink not followed. On Windows a link lists
/// as what it points to — a directory link (symlink or junction) as a
/// directory: LLVM's listing keeps no reparse attribute.
struct Entry {
    std::string path;
    llvm::sys::fs::file_type type;
};

/// The entries of a directory, in the order the system lists them.
std::expected<std::vector<Entry>, std::error_code> read_dir(llvm::StringRef dir);

/// Walk the tree under `root` depth first, symlinks not followed (see
/// Entry): `visit` sees every entry and answers, for a directory, whether
/// to walk into it. A directory that cannot be read is skipped, with a
/// warning unless it is missing.
void walk(llvm::StringRef root, llvm::function_ref<bool(const Entry&)> visit);

std::error_code create_directories(llvm::StringRef path);

/// Write a file whole, replacing what it held.
std::error_code write(llvm::StringRef path, llvm::StringRef content);

/// Write a file whole through a sibling renamed over it: a reader sees the
/// old bytes or the new, never a part, and concurrent writers never mix.
std::error_code write_atomic(llvm::StringRef path, llvm::StringRef content);

std::error_code rename(llvm::StringRef from, llvm::StringRef to);

/// Remove a file or an empty directory; a missing one is no error. Unlike
/// llvm::sys::fs::remove, a file another process still maps goes on
/// Windows as well: its name disappears at once and its content once the
/// last mapping is gone, as on POSIX. Workers keep PCH blobs mapped across
/// compiles, and the store retracts and replaces those blobs under them.
std::error_code remove(llvm::StringRef path);

/// Remove a tree; a missing one is no error. Symlinks — a symlinked root
/// included — are removed without following them, so the recursion never
/// escapes into a link's target; on Windows that holds for file links only
/// (see Entry). Not llvm::sys::fs::remove_directories: on Windows that runs
/// shell COM, which initializes an apartment on the calling thread and
/// silently does nothing when that fails.
std::error_code remove_all(llvm::StringRef path);

/// Create a new empty file in the system's temporary directory, named
/// `prefix`-<random>.`suffix`.
std::expected<std::string, std::error_code> temp_file(llvm::StringRef prefix,
                                                      llvm::StringRef suffix);

/// Keep the mapping of a PCH from clice's store for the process's later
/// compiles: Windows pages every fresh mapping in fault by fault, on every
/// compile. The store gives each PCH build a name of its own and never
/// rewrites one, so a kept mapping stays the file's bytes. Nothing else
/// qualifies — a PCM is rebuilt under the name it had — since on Windows a
/// held mapping stops anyone from replacing the file.
void keep_mapped(llvm::StringRef path);

/// The file system one compile sees: the disk, each file served the way
/// read() serves it — sources as text, `#embed` data, PCH and PCM files
/// as bytes. Clang sets its working directory, so every compile gets its
/// own; an overlay wraps a fresh one.
class View : public llvm::vfs::ProxyFileSystem {
public:
    View() : ProxyFileSystem(llvm::vfs::createPhysicalFileSystem()) {}

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

    StatusBatch statuses;
};

}  // namespace vfs

}  // namespace clice
