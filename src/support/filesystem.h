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

/// Remove a file or an empty directory. Unlike llvm::sys::fs::remove, a
/// file another process still maps goes on Windows as well: its name
/// disappears at once and its content once the last mapping is gone, as
/// on POSIX. Workers keep PCH blobs mapped across compiles, and the
/// store retracts and replaces those blobs under them.
std::error_code remove(const llvm::Twine& path, bool ignore_non_existing = true);

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
        return remove(target);
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
        } else if(auto remove_ec = remove(it->path())) {
            return remove_ec;
        }
    }
    if(ec) {
        // A missing root is fine: there is simply nothing to remove.
        return ec == std::errc::no_such_file_or_directory ? std::error_code() : ec;
    }
    return remove(target);
}

}  // namespace fs

}  // namespace clice
