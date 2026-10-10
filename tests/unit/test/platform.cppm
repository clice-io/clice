module;

#include "modules/prelude.h"

module clice:tests.unit.test.platform;

import :vfs.path;

namespace clice::testing {

#ifdef _WIN32
constexpr inline bool Windows = true;
#else
constexpr inline bool Windows = false;
#endif

#ifdef __linux__
constexpr inline bool Linux = true;
#else
constexpr inline bool Linux = false;
#endif

/// A checked-in tree: `fallback` of the working directory (the repository
/// root, where the tasks run the tests: a build that may be cached across
/// checkouts names none of them), unless `env` names another, the way
/// CLICE_EXECUTABLE points the TypeScript suites at another build. Absolute
/// and dot-free with native separators, the spelling the database loader
/// produces for paths anchored under it.
inline std::string checkout_dir(const char* env, const char* fallback) {
    llvm::SmallString<256> dir;
    if(const char* value = std::getenv(env)) {
        dir = value;
    } else {
        dir = fallback;
    }
    llvm::sys::fs::make_absolute(dir);
    llvm::sys::path::remove_dots(dir, /*remove_dot_dot=*/true);
    return std::string(dir);
}

/// The unit tests' own fixtures, tests/unit/data.
inline std::string data_dir() {
    return checkout_dir("CLICE_TEST_DATA_DIR", CLICE_TESTS_DATA_DIR);
}

/// The sample projects every suite shares, samples/.
inline std::string samples_dir() {
    return checkout_dir("CLICE_TEST_SAMPLES_DIR", CLICE_TESTS_SAMPLES_DIR);
}

class TestVFS : public llvm::vfs::InMemoryFileSystem {
public:
    TestVFS() {
        setCurrentWorkingDirectory(root());
    }

    const static char* root() {
#ifdef _WIN32
        return "c:/clice-test";
#else
        return "/clice-test";
#endif
    }

    /// root() + relative → absolute path, spelled the way the file table
    /// names a file (path::canonicalize); an absolute path stays as is (a
    /// file that must live outside the root, e.g. inside a cache store's
    /// directory).
    static std::string path(llvm::StringRef relative) {
        if(llvm::sys::path::is_absolute(relative)) {
            return relative.str();
        }
        llvm::SmallString<128> joined;
        llvm::sys::path::append(joined, root(), relative);
        std::string result(joined);
        path::canonicalize(result);
        return result;
    }

    /// Add a file with an optional content (relative path, auto-prefixed
    /// with root(), or absolute).
    void add(llvm::StringRef relative, llvm::StringRef content = {}) {
        auto p = path(relative);
        addFile(p, 0, llvm::MemoryBuffer::getMemBufferCopy(content, p));
    }
};

}  // namespace clice::testing
