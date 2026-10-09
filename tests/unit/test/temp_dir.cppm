module;

#include "modules/prelude.h"

module clice:tests.unit.test.temp_dir;

import :vfs.file_system;

namespace clice::testing {

/// RAII helper for a temporary directory tree.
///
/// Creates a unique temporary directory on construction and removes it
/// (recursively) on destruction, or when a failed ZASSERT ends the test.  Provides helpers for
/// building paths, creating sub-directories, and writing files — used across multiple test suites
/// that need real filesystem state.
///
/// Also serves as a cross-platform source of absolute paths: on Windows
/// the root includes a drive letter, so `path("x")` is absolute everywhere.
struct TempDir {
    llvm::SmallString<128> root;

    TempDir(llvm::StringRef prefix = "clice-test") {
        llvm::sys::fs::createUniqueDirectory(prefix, root);
        // Canonicalize the root so path() spellings match what the compiler
        // reports (CompilationUnit::file_path realpaths every file): the
        // macOS temp dir lives behind the /var -> /private/var symlink and
        // Windows runners hand out 8.3 short names — either would make
        // every path-keyed index lookup in tests miss.
        llvm::SmallString<128> real;
        if(!llvm::sys::fs::real_path(root, real)) {
            root = real;
        }
    }

    ~TempDir() {
        vfs::remove_all(root.str());
    }

    kota::zest::FatalHook cleanup{[this] { vfs::remove_all(root.str()); }};

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /// Build an absolute path under this temporary root.
    std::string path(llvm::StringRef relative) const {
        llvm::SmallString<256> result(root);
        llvm::sys::path::append(result, relative);
        llvm::sys::path::native(result);
        return std::string(result);
    }

    /// Like path(), but returns a `const char*` whose lifetime is tied to
    /// this TempDir.  Useful for building `ArrayRef<const char*>` argument
    /// lists without manual lifetime management.
    const char* c_path(llvm::StringRef relative) {
        pool.push_back(path(relative));
        return pool.back().c_str();
    }

    /// Create a sub-directory (and any parents).
    void mkdir(llvm::StringRef relative) {
        llvm::sys::fs::create_directories(path(relative));
    }

    /// Create a file with optional content (parent dirs created automatically).
    void touch(llvm::StringRef relative, llvm::StringRef content = "") {
        auto p = path(relative);
        llvm::sys::fs::create_directories(llvm::sys::path::parent_path(p));
        std::error_code ec;
        llvm::raw_fd_ostream out(p, ec);
        if(!ec) {
            out << content;
        }
    }

private:
    /// Pool for strings returned by c_path(). std::deque guarantees that
    /// existing elements are not moved when new ones are appended.
    std::deque<std::string> pool;
};

/// Rewrite a file's or directory's mtime, for freshness tests that simulate
/// backdated or preserved timestamps (rsync -t, git-restore-mtime, a project
/// tree older than the server). Returns false on failure so callers can
/// assert.
bool set_file_mtime(llvm::StringRef path, std::int64_t mtime_ns);

/// A file's current mtime in nanoseconds, or -1 when it cannot be stat'ed.
inline std::int64_t file_mtime_ns(llvm::StringRef path) {
    auto status = vfs::status(path);
    return status ? status->stamp.mtime_ns : -1;
}

/// A file's bytes, or nullopt when it cannot be read.
inline std::optional<std::string> read_file(llvm::StringRef path) {
    auto buffer = vfs::read(path, vfs::Read::Bytes);
    if(!buffer) {
        return std::nullopt;
    }
    return (*buffer)->getBuffer().str();
}

}  // namespace clice::testing
