#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include "support/format.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

namespace clice {

namespace path {

using namespace llvm::sys::path;

template <typename... Args>
std::string join(Args&&... args) {
    llvm::SmallString<128> path;
    ((path::append(path, std::forward<Args>(args))), ...);
    return path.str().str();
}

/// Path identity on Windows is separator-agnostic and drive-case
/// insensitive; LSP clients (vscode-uri) key documents by the
/// lowercase-drive, forward-slash spelling, so that spelling is the
/// canonical form for identity and URI emission. On POSIX identity is
/// the raw bytes — '\' and "C:" are ordinary filename characters — and
/// canonicalization must not touch paths there.

/// True when `p` deviates from the Windows canonical spelling.
inline bool needs_canonical(llvm::StringRef p) {
    return p.contains('\\') || (p.size() >= 2 && p[1] == ':' && llvm::isUpper(p[0]));
}

/// The rewrite itself, in place. Platform-independent on purpose so the
/// logic is unit-testable on any host; production code reaches it only
/// through the gated entry points below.
inline void make_canonical(llvm::MutableArrayRef<char> p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    if(p.size() >= 2 && p[1] == ':' && llvm::isUpper(p[0])) {
        p[0] = llvm::toLower(p[0]);
    }
}

/// Canonical view of `p`: on Windows, `p` itself when already canonical,
/// otherwise the rewrite materialized in `storage`; on POSIX always `p`.
inline llvm::StringRef canonical(llvm::StringRef p,
                                 [[maybe_unused]] llvm::SmallVectorImpl<char>& storage) {
#ifdef _WIN32
    if(needs_canonical(p)) {
        storage.assign(p.begin(), p.end());
        make_canonical(storage);
        return llvm::StringRef(storage.data(), storage.size());
    }
#endif
    return p;
}

/// In-place canonicalization of an owned string (config dirs, the
/// workspace root). No-op on POSIX.
inline void canonicalize([[maybe_unused]] std::string& p) {
#ifdef _WIN32
    if(needs_canonical(p)) {
        make_canonical(llvm::MutableArrayRef(p.data(), p.size()));
    }
#endif
}

}  // namespace path

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

/// The newest mtime (ns) a file may carry and still be provably untouched
/// since the reference moment `at_ms` (a build start, or "now" for
/// content read on the spot).
constexpr std::int64_t stat_baseline_before_ns(std::int64_t at_ms) {
    return at_ms * 1'000'000 - mtime_guard_ns;
}

inline std::expected<void, std::error_code> write(llvm::StringRef path, llvm::StringRef content) {
    std::error_code EC;
    llvm::raw_fd_ostream os(path, EC, llvm::sys::fs::OF_None);
    if(EC) {
        return std::unexpected(EC);
    }
    os << content;
    os.flush();
    return std::expected<void, std::error_code>();
}

inline std::expected<std::string, std::error_code> read(llvm::StringRef path) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if(!buffer) {
        return std::unexpected(buffer.getError());
    }
    return buffer.get()->getBuffer().str();
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

namespace vfs = llvm::vfs;

class ThreadSafeFS : public vfs::ProxyFileSystem {
public:
    explicit ThreadSafeFS() : ProxyFileSystem(vfs::createPhysicalFileSystem()) {}

    class VolatileFile : public vfs::File {
    public:
        explicit VolatileFile(std::unique_ptr<vfs::File> wrapped) : wrapped(std::move(wrapped)) {
            assert(this->wrapped);
        }

        llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getBuffer(const llvm::Twine& Name,
                                                                     int64_t FileSize,
                                                                     bool RequiresNullTerminator,
                                                                     bool /*IsVolatile*/) override {
            return wrapped->getBuffer(Name,
                                      FileSize,
                                      RequiresNullTerminator,
                                      /*IsVolatile=*/true);
        }

        llvm::ErrorOr<vfs::Status> status() override {
            return wrapped->status();
        }

        llvm::ErrorOr<std::string> getName() override {
            return wrapped->getName();
        }

        std::error_code close() override {
            return wrapped->close();
        }

    private:
        std::unique_ptr<File> wrapped;
    };

    llvm::ErrorOr<std::unique_ptr<vfs::File>> openFileForRead(const llvm::Twine& InPath) override {
        llvm::SmallString<128> Path;
        InPath.toVector(Path);

        auto file = getUnderlyingFS().openFileForRead(Path);
        if(!file) {
            return file;
        }

        llvm::StringRef filename = path::filename(Path);
        if(filename.ends_with(".pch")) {
            return file;
        }
        return std::make_unique<VolatileFile>(std::move(*file));
    }
};

}  // namespace clice
