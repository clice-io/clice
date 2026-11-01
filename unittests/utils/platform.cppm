module clice:unittests.utils.platform;

import :support.prelude;
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

/// The environment variable `name` as a test sets it, `value` or unset,
/// until the guard ends.
struct ScopedEnv {
    ScopedEnv(std::string name, std::optional<std::string> value) : name(std::move(name)) {
        saved = sys::get_env(previous, this->name);
        ZASSERT(!(value ? sys::set_env(this->name, *value) : sys::unset_env(this->name)));
    }

    ScopedEnv(const ScopedEnv&) = delete;

    ~ScopedEnv() {
        if(saved) {
            sys::set_env(name, previous);
        } else {
            sys::unset_env(name);
        }
    }

    std::string name;

    std::string previous;

    bool saved = false;
};

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
        if(path::is_absolute(relative)) {
            return relative.str();
        }
        auto result = path::join(root(), relative);
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
