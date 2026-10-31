module clice:unittests.utils.temp_dir;

import :support.prelude;
import :vfs.file_system;
import :vfs.path;

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
    TempDir(llvm::StringRef prefix = "clice-test") : directory(create(prefix)) {}

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    fs::TempDirectory directory;

    /// As the OS names it, so path() spells files as the compiler reports
    /// them (CompilationUnit::file_path realpaths every file): the macOS
    /// temp dir lives behind the /var -> /private/var symlink and Windows
    /// runners hand out 8.3 short names — either would make every
    /// path-keyed index lookup in tests miss.
    std::string root{directory.path()};

    zest::FatalHook cleanup{[this] { directory.remove(); }};

    /// Build an absolute path under this temporary root.
    std::string path(llvm::StringRef relative) const {
        std::string result = root;
        path::push(result, relative);
#ifdef _WIN32
        path::to_backslash(result);
#endif
        return result;
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
        fs::create_directories(path(relative));
    }

    /// Create a file with optional content (parent dirs created automatically).
    void touch(llvm::StringRef relative, llvm::StringRef content = "") {
        auto p = path(relative);
        fs::create_directories(path::parent(p));
        fs::write_file(p, content);
    }

private:
    static fs::TempDirectory create(llvm::StringRef prefix) {
        std::string temp;
        sys::temp_dir(temp);
        return fs::TempDirectory::create(temp, prefix.str() + "-").value();
    }

    /// Pool for strings returned by c_path(). std::deque guarantees that
    /// existing elements are not moved when new ones are appended.
    std::deque<std::string> pool;
};

/// Rewrite a file's or directory's mtime, for freshness tests that simulate
/// backdated or preserved timestamps (rsync -t, git-restore-mtime, a project
/// tree older than the server). Returns false on failure so callers can
/// assert.
inline bool set_file_mtime(llvm::StringRef path, std::int64_t mtime_ns) {
    using namespace chrono;
    return !fs::set_times(path, {.mtime = sys_time<nanoseconds>(nanoseconds(mtime_ns))});
}

/// A file's current mtime in nanoseconds, or -1 when it cannot be stat'ed.
inline std::int64_t file_mtime_ns(llvm::StringRef path) {
    auto status = fs::status(path);
    return status ? vfs::Stamp::of(*status).mtime_ns : -1;
}

/// A file's bytes, or nullopt when it cannot be read.
inline std::optional<std::string> read_file(llvm::StringRef path) {
    std::string bytes;
    if(fs::read_file(bytes, path)) {
        return std::nullopt;
    }
    return bytes;
}

}  // namespace clice::testing
