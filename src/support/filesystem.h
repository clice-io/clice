#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

namespace clice {

namespace fs {

using namespace llvm::sys::fs;

using llvm::sys::fs::createTemporaryFile;

inline std::expected<std::string, std::error_code> createTemporaryFile(llvm::StringRef prefix,
                                                                       llvm::StringRef suffix) {
    llvm::SmallString<128> path;
    auto error = llvm::sys::fs::createTemporaryFile(prefix, suffix, path);
    if(error) {
        return std::unexpected(error);
    }
    return path.str().str();
}

/// A file's mtime as nanoseconds since epoch — the resolution every
/// freshness baseline in the project stores.
inline std::int64_t mtime_ns(const llvm::sys::fs::file_status& status) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               status.getLastModificationTime().time_since_epoch())
        .count();
}

/// Filesystem mtime-granularity guard for freshness baselines: a stat fast
/// path is only recorded for a file whose mtime precedes the reference
/// moment by at least this much. Closer to it, a coarse-granularity
/// filesystem (FAT stores 2s mtimes, several network filesystems whole
/// seconds) could stamp a write the reference never saw with a timestamp
/// from before it — so those files re-earn their fast path through one
/// hash comparison instead.
constexpr inline std::int64_t mtime_guard_ns = 2'000'000'000;

/// Whether the filesystem's UniqueID identifies a file rather than a
/// path. Windows file IDs are not file-stable everywhere (ReFS dev
/// drives report path-derived values, and handle- and path-derived
/// stats of one file can disagree), so identity-based staleness
/// defenses are POSIX-only.
#ifdef _WIN32
constexpr inline bool stable_file_ids = false;
#else
constexpr inline bool stable_file_ids = true;
#endif

/// Metadata needed to validate cached bytes or a directory listing. Unlike
/// LLVM's full status, this does not compute a canonical-path identity on
/// Windows, where the cache deliberately does not use filesystem IDs.
struct FileMetadata {
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::uint64_t uid_device = 0;
    std::uint64_t uid_file = 0;

    FileMetadata() = default;

    FileMetadata(const llvm::sys::fs::file_status& status) :
        size(status.getSize()), mtime_ns(fs::mtime_ns(status)) {
        if constexpr(stable_file_ids) {
            uid_device = status.getUniqueID().getDevice();
            uid_file = status.getUniqueID().getFile();
        }
    }
};

/// Follow links, like fs::status. Reparse points and unusual Windows paths
/// fall back to LLVM. Use full status when file type or identity is needed.
std::error_code file_metadata(const llvm::Twine& path, FileMetadata& result);

/// Query the already-open file, keeping read observations bound to its handle.
std::error_code file_metadata(llvm::sys::fs::file_t file, FileMetadata& result);

/// The newest mtime (ns) a file may carry and still be provably untouched
/// since the reference moment `at_ms` (a build start, or "now" for
/// content read on the spot).
constexpr std::int64_t stat_baseline_before_ns(std::int64_t at_ms) {
    return at_ms * 1'000'000 - mtime_guard_ns;
}

/// Whether a stat taken now carrying `mtime_ns` may vouch for what it
/// describes beyond this moment.
inline bool settled(std::int64_t mtime_ns) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    return mtime_ns <= stat_baseline_before_ns(now_ms);
}

inline std::expected<void, std::error_code> write(llvm::StringRef path, llvm::StringRef content) {
    std::error_code EC;
    llvm::raw_fd_ostream os(path, EC, llvm::sys::fs::OF_None);
    if(EC) {
        return std::unexpected(EC);
    }
    os << content;
    os.flush();
    if(os.has_error()) {
        auto error = os.error();
        os.clear_error();
        return std::unexpected(error);
    }
    return {};
}

inline std::expected<void, std::error_code> rename(llvm::StringRef from, llvm::StringRef to) {
    auto error = llvm::sys::fs::rename(from, to);
    if(error) {
        return std::unexpected(error);
    }
    return std::expected<void, std::error_code>();
}

/// Recursively remove a directory tree using plain filesystem primitives.
/// Use this instead of llvm::sys::fs::remove_directories: on Windows that
/// is implemented over shell COM (CoInitializeEx + IFileOperation), which
/// initializes apartment COM on the calling thread and silently no-ops
/// when that fails — unsuitable for server threads.  This mirrors the
/// recursion LLVM's own Unix implementation uses.  Symlinks — including a
/// symlinked root — are removed without following them, so the recursion
/// can never escape into the link's target.  Returns the first error; a
/// missing path is not an error.
inline std::error_code remove_all(llvm::StringRef target) {
    llvm::sys::fs::file_status target_status;
    if(auto status_ec = llvm::sys::fs::status(target, target_status, /*follow=*/false)) {
        return status_ec == std::errc::no_such_file_or_directory ? std::error_code() : status_ec;
    }
    if(target_status.type() != llvm::sys::fs::file_type::directory_file) {
        return llvm::sys::fs::remove(target, /*IgnoreNonExisting=*/true);
    }

    std::error_code ec;
    for(llvm::sys::fs::directory_iterator it(target, ec, /*follow_symlinks=*/false), end;
        !ec && it != end;
        it.increment(ec)) {
        auto status = it->status();
        if(!status) {
            return status.getError();
        }
        if(llvm::sys::fs::is_directory(*status)) {
            if(auto sub_ec = remove_all(it->path())) {
                return sub_ec;
            }
        } else if(auto remove_ec = llvm::sys::fs::remove(it->path(), /*IgnoreNonExisting=*/true)) {
            return remove_ec;
        }
    }
    if(ec) {
        // A missing root is fine: there is simply nothing to remove.
        return ec == std::errc::no_such_file_or_directory ? std::error_code() : ec;
    }
    return llvm::sys::fs::remove(target, /*IgnoreNonExisting=*/true);
}

}  // namespace fs

}  // namespace clice
