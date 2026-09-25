#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <compare>
#include <concepts>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include "support/format.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"

namespace clice {

namespace path {

using namespace llvm::sys::path;

/// Whether `p` is spelled from a root, so no anchor may be prepended:
/// absolute in the native style, or rooted without a drive (`/x`), which
/// Windows resolves against the current drive and `is_absolute` rejects.
inline bool is_rooted(llvm::StringRef p) {
    return is_absolute(p) || has_root_directory(p);
}

/// Whether `p` is `root` or lies under it.
inline bool under(llvm::StringRef p, llvm::StringRef root) {
    return p == root ||
           (p.starts_with(root) && (root.ends_with("/") || is_separator(p[root.size()])));
}

/// Visit `start` and its ancestors, nearest first, until `visit` returns
/// false or `stop` has been visited (the filesystem root when empty).
inline void walk_ancestors(llvm::StringRef start,
                           llvm::StringRef stop,
                           llvm::function_ref<bool(llvm::StringRef)> visit) {
    while(stop.size() > 1 && is_separator(stop.back())) {
        stop = stop.drop_back();
    }
    for(llvm::StringRef dir = start; !dir.empty();) {
        if(!visit(dir) || dir == stop) {
            return;
        }
        auto parent = parent_path(dir);
        if(parent.size() == dir.size()) {
            return;
        }
        dir = parent;
    }
}

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

struct FileTable;
class CanonicalPath;

/// A path naming a file or directory by its identity, the way the file
/// table does: the symlinks of its longest existing prefix resolved, the
/// rest appended as spelled, `.`/`..` removed, canonically spelled. Two
/// spellings of one file compare equal whether it exists yet or not.
///
/// Only resolution (CanonicalPath's constructor) and the file table make
/// one, and one never compares with a plain string: a spelling taken for
/// an identity is a compile error. Either reads as a plain string wherever
/// a spelling will do.
class CanonicalRef {
public:
    CanonicalRef() = default;

    operator llvm::StringRef() const {
        return text;
    }

    /// For LLVM's file APIs; points into this object, so it lives for the
    /// call it is passed to.
    operator llvm::Twine() const {
        return llvm::Twine(text);
    }

    explicit operator std::string() const {
        return text.str();
    }

    std::string str() const {
        return text.str();
    }

    /// Null-terminated.
    const char* data() const {
        return text.data();
    }

    std::size_t size() const {
        return text.size();
    }

    bool empty() const {
        return text.empty();
    }

    /// The directory holding it, itself an identity; empty at a root.
    CanonicalPath parent() const;

    /// A path a directory walk from here reached without following a
    /// symlink, itself an identity: every component below this one is a
    /// real directory entry.
    CanonicalPath entry(llvm::StringRef path) const;

private:
    friend class CanonicalPath;
    friend struct FileTable;

    explicit CanonicalRef(llvm::StringRef text) : text(text) {}

    llvm::StringRef text;
};

class CanonicalPath {
public:
    CanonicalPath() = default;

    /// The identity of what `spelled` names.
    explicit CanonicalPath(llvm::StringRef spelled);

    CanonicalPath(CanonicalRef ref) : text(ref.str()) {}

    operator CanonicalRef() const {
        return CanonicalRef(text);
    }

    operator llvm::StringRef() const {
        return text;
    }

    /// For LLVM's file APIs; points into this object, so it lives for the
    /// call it is passed to.
    operator llvm::Twine() const {
        return llvm::Twine(text);
    }

    explicit operator std::string() const {
        return text;
    }

    const std::string& str() const {
        return text;
    }

    std::size_t size() const {
        return text.size();
    }

    bool empty() const {
        return text.empty();
    }

private:
    friend class CanonicalRef;

    struct Resolved {};

    CanonicalPath(Resolved, llvm::StringRef text) : text(text) {}

    std::string text;
};

/// An identity: a CanonicalRef, a CanonicalPath, or a type wrapping one
/// by inheritance (a reflected configuration field).
template <typename T>
concept Canonical = std::same_as<T, CanonicalRef> || std::derived_from<T, CanonicalPath>;

template <Canonical L, Canonical R>
bool operator==(const L& lhs, const R& rhs) {
    return llvm::StringRef(lhs) == llvm::StringRef(rhs);
}

template <Canonical L, Canonical R>
std::strong_ordering operator<=>(const L& lhs, const R& rhs) {
    return llvm::StringRef(lhs).compare(llvm::StringRef(rhs)) <=> 0;
}

/// An identity compares with an identity only.
template <typename L, typename R>
    requires (Canonical<L> != Canonical<R>)
bool operator==(const L& lhs, const R& rhs) = delete;

template <typename L, typename R>
    requires (Canonical<L> != Canonical<R>)
std::strong_ordering operator<=>(const L& lhs, const R& rhs) = delete;

namespace path {

/// Whether the identity `p` is `root` or lies under it.
template <Canonical P, Canonical R>
bool under(const P& p, const R& root) {
    return under(llvm::StringRef(p), llvm::StringRef(root));
}

/// An identity lies under an identity only.
template <typename P, typename R>
    requires (Canonical<P> != Canonical<R>)
bool under(const P& p, const R& root) = delete;

}  // namespace path

inline CanonicalPath CanonicalRef::parent() const {
    auto dir = path::parent_path(text);
    return CanonicalPath(CanonicalPath::Resolved{},
                         dir.size() < text.size() ? dir : llvm::StringRef());
}

inline CanonicalPath CanonicalRef::entry(llvm::StringRef path) const {
    assert(path::under(path, text));
    return CanonicalPath(CanonicalPath::Resolved{}, path);
}

inline CanonicalPath::CanonicalPath(llvm::StringRef spelled) {
    llvm::SmallString<256> real;
    llvm::StringRef existing = spelled;
    while(llvm::sys::fs::real_path(existing, real)) {
        auto parent = path::parent_path(existing);
        if(parent.empty() || parent.size() == existing.size()) {
            text = spelled.str();
            return;
        }
        existing = parent;
    }
    real += spelled.drop_front(existing.size());
    // The unresolved tail may still climb (`missing/../cache`).
    path::remove_dots(real, /*remove_dot_dot=*/true);
    text = std::string(real);
    path::canonicalize(text);
}

}  // namespace clice

template <clice::Canonical T>
struct std::formatter<T> : std::formatter<llvm::StringRef> {
    template <typename FormatContext>
    auto format(const T& value, FormatContext& ctx) const {
        return std::formatter<llvm::StringRef>::format(llvm::StringRef(value), ctx);
    }
};

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

/// A source file's text as every part of clice sees it: its bytes without
/// a leading UTF-8 byte order mark — the text an editor shows and sends.
/// Clang skips the mark as well, but would count it in every offset.
inline llvm::StringRef without_bom(llvm::StringRef bytes) {
    llvm::StringRef text = bytes;
    text.consume_front("\xEF\xBB\xBF");
    return text;
}

class ThreadSafeFS : public vfs::ProxyFileSystem {
public:
    explicit ThreadSafeFS() : ProxyFileSystem(vfs::createPhysicalFileSystem()) {}

    /// Serves its file's text (see without_bom), the size its status
    /// reports agreeing with the bytes read, as clang checks.
    class VolatileFile : public vfs::File {
    public:
        explicit VolatileFile(std::unique_ptr<vfs::File> wrapped) : wrapped(std::move(wrapped)) {
            assert(this->wrapped);
        }

        llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
            getBuffer(const llvm::Twine& Name, int64_t, bool, bool) override {
            if(auto error = load(Name)) {
                return error;
            }
            return std::move(buffer);
        }

        llvm::ErrorOr<vfs::Status> status() override {
            auto status = wrapped->status();
            if(!status || status->getType() != llvm::sys::fs::file_type::regular_file) {
                return status;
            }
            if(auto error = load(status->getName())) {
                return error;
            }
            return vfs::Status::copyWithNewSize(*status, buffer->getBufferSize());
        }

        llvm::ErrorOr<std::string> getName() override {
            return wrapped->getName();
        }

        std::error_code close() override {
            return wrapped->close();
        }

    private:
        /// Read the whole file once, as a volatile snapshot, dropping the
        /// mark.
        std::error_code load(const llvm::Twine& name) {
            if(buffer) {
                return {};
            }
            auto read = wrapped->getBuffer(name, -1, true, /*IsVolatile=*/true);
            if(!read) {
                return read.getError();
            }
            buffer = std::move(*read);
            auto text = without_bom(buffer->getBuffer());
            if(text.size() != buffer->getBufferSize()) {
                buffer = llvm::MemoryBuffer::getMemBufferCopy(text, buffer->getBufferIdentifier());
            }
            return {};
        }

        std::unique_ptr<File> wrapped;
        std::unique_ptr<llvm::MemoryBuffer> buffer;
    };

    llvm::ErrorOr<vfs::Status> status(const llvm::Twine& path) override {
        auto status = getUnderlyingFS().status(path);
        if(!status || status->getType() != llvm::sys::fs::file_type::regular_file ||
           status->getSize() < 3 || skips(status->getName())) {
            return status;
        }
        llvm::SmallString<256> absolute;
        path.toVector(absolute);
        if(getUnderlyingFS().makeAbsolute(absolute) || !starts_with_bom(absolute)) {
            return status;
        }
        return vfs::Status::copyWithNewSize(*status, status->getSize() - 3);
    }

    llvm::ErrorOr<std::unique_ptr<vfs::File>> openFileForRead(const llvm::Twine& InPath) override {
        llvm::SmallString<128> Path;
        InPath.toVector(Path);

        auto file = getUnderlyingFS().openFileForRead(Path);
        if(!file || skips(Path)) {
            return file;
        }
        return std::make_unique<VolatileFile>(std::move(*file));
    }

private:
    /// Built artifacts are served as they are.
    static bool skips(llvm::StringRef path) {
        return path::filename(path).ends_with(".pch");
    }

    static bool starts_with_bom(llvm::StringRef path) {
        auto fd = llvm::sys::fs::openNativeFileForRead(path);
        if(!fd) {
            llvm::consumeError(fd.takeError());
            return false;
        }
        char head[3];
        auto read = llvm::sys::fs::readNativeFile(*fd, head);
        llvm::sys::fs::closeFile(*fd);
        if(!read) {
            llvm::consumeError(read.takeError());
            return false;
        }
        return *read == 3 && without_bom(llvm::StringRef(head, 3)).empty();
    }
};

}  // namespace clice
